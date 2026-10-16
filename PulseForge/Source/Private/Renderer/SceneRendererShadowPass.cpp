#include "Renderer/SceneRendererShadowPass.h"

#include "Renderer/AssetCache/MeshAssetCache.h"
#include "Renderer/SceneRendererLocalLightingState.h"
#include "Renderer/SceneRendererMaterialBindingCache.h"
#include "Renderer/SceneRendererPipelineCache.h"
#include "Renderer/SceneRendererShadowTargetCache.h"
#include "Renderer/RenderTargetFrameScope.h"

#include <algorithm>
#include <cmath>
#include <span>

namespace PulseForge
{
	namespace
	{
		SceneRendererError MakeShadowResourceError(std::string Message)
		{
			return { SceneRendererErrorCode::ResourceCreationFailed, {}, {}, std::move(Message) };
		}

		SceneRendererError MakeShadowDrawError(std::string Message)
		{
			return { SceneRendererErrorCode::DrawFailed, {}, {}, std::move(Message) };
		}

	} // namespace

	std::expected<void, SceneRendererError> SceneRenderer::ShadowPass::Render(const Resources& Resources)
	{
		m_Statistics = {};
		if (auto Lighting = PrepareLocalLighting(Resources); !Lighting) return std::unexpected(std::move(Lighting.error()));

		if (Resources.Snapshot && Resources.Snapshot->DirectionalLight && Resources.Snapshot->DirectionalLight->CastShadows &&
			!Resources.Snapshot->Meshes.empty())
		{
			if (auto Directional = RenderDirectionalCascades(Resources); !Directional)
				return std::unexpected(std::move(Directional.error()));
		}

		return RenderSpotlights(Resources, m_Statistics);
	}

	std::expected<void, SceneRendererError> SceneRenderer::ShadowPass::PrepareLocalLighting(const Resources& Resources)
	{
		auto Plan = Resources.LocalLighting.BuildPlan(Resources.Snapshot);
		if (!Plan) return std::unexpected(std::move(Plan.error()));

		if (Plan->RequiresSpotlightResources)
		{
			if (auto TargetResult = Resources.Targets.EnsureSpotlightMaps(Resources.SpotlightLayout, Resources.ShadowSampler);
				!TargetResult)
				return std::unexpected(std::move(TargetResult.error()));
			if (auto Pipeline = Resources.Pipelines.EnsureShadowPipeline(1.0f); !Pipeline)
				return std::unexpected(std::move(Pipeline.error()));
		}
		if (Plan->RequiresPointShadowResources)
		{
			if (auto TargetResult = Resources.Targets.EnsurePointLightCubemaps(Resources.PointLightLayout, Resources.ShadowSampler);
				!TargetResult)
				return std::unexpected(std::move(TargetResult.error()));
			if (auto Pipeline = Resources.Pipelines.EnsurePointShadowPipeline(false); !Pipeline)
				return std::unexpected(std::move(Pipeline.error()));
			if (auto Pipeline = Resources.Pipelines.EnsurePointShadowPipeline(true); !Pipeline)
				return std::unexpected(std::move(Pipeline.error()));
		}

		return Resources.LocalLighting.Commit();
	}

	std::expected<void, SceneRendererError> SceneRenderer::ShadowPass::RenderDirectionalCascades(const Resources& Resources)
	{
		if (!Resources.Cascades) return std::unexpected(MakeShadowDrawError("Prepared directional shadow cascades are unavailable"));
		const int32_t RasterBias = static_cast<int32_t>(std::lround(Resources.Snapshot->DirectionalLight->ShadowBias));

		for (size_t CascadeIndex = 0; CascadeIndex < Resources.Cascades->Cascades.size(); ++CascadeIndex)
		{
			const GraphicsResult Begin = Resources.Runtime.BeginRenderTarget(Resources.Targets.DirectionalTarget(CascadeIndex));
			if (!Begin) return std::unexpected(MakeShadowDrawError("Could not begin shadow cascade: " + Begin.error().Message));
			RenderTargetFrameScope TargetScope(Resources.Runtime);
			for (const SceneMeshInstance& Instance : Resources.Snapshot->Meshes)
			{
				auto Material = Resources.MaterialBindings.ResolveMaterialDescription(Instance);
				if (!Material) return std::unexpected(std::move(Material.error()));
				if ((*Material)->AlphaMode == MaterialAlphaMode::Blend) continue;
				auto MaterialBinding = Resources.MaterialBindings.ResolveMaterialBinding(Instance, Resources.BindingEnvironmentAsset);
				if (!MaterialBinding) return std::unexpected(std::move(MaterialBinding.error()));
				if (auto PipelineResult = Resources.Pipelines.EnsureShadowPipeline(
						Resources.Snapshot->DirectionalLight->ShadowBias, (*Material)->DoubleSided);
					!PipelineResult)
					return std::unexpected(std::move(PipelineResult.error()));
				const GraphicsPipeline* Pipeline = Resources.Pipelines.FindShadowPipeline(RasterBias, (*Material)->DoubleSided);
				if (!Pipeline) return std::unexpected(MakeShadowResourceError("Directional alpha-tested shadow pipeline was not prepared"));
				auto Mesh = Resources.Meshes.GetOrLoad(Instance.MeshAsset);
				if (!Mesh) return std::unexpected(MakeShadowDrawError("Shadow pass mesh became unavailable: " + Mesh.error().Message));
				const glm::mat4 ShadowTransform = Resources.Cascades->Cascades[CascadeIndex].ViewProjection * Instance.WorldTransform;
				const auto Update =
					Resources.Runtime.WriteBuffer(*Resources.ShadowObjectConstants, 0, std::as_bytes(std::span(&ShadowTransform, 1)));
				if (!Update)
					return std::unexpected(MakeShadowDrawError("Could not update shadow object constants: " + Update.error().Message));
				const std::array<const BindingSet*, 2> Bindings = { *MaterialBinding, Resources.ShadowObjectBindings.get() };
				const DrawIndexedArguments Arguments{ Mesh->get().GetIndexCount(), 1, 0, 0 };
				const GraphicsResult Draw = Resources.Runtime.DrawIndexed(*Pipeline, Mesh->get(), Arguments, Bindings);
				if (!Draw) return std::unexpected(MakeShadowDrawError("Could not draw shadow caster: " + Draw.error().Message));
			}
			const GraphicsResult End = TargetScope.End();
			if (!End) return std::unexpected(MakeShadowDrawError("Could not end shadow cascade: " + End.error().Message));
		}
		return {};
	}

	std::expected<void, SceneRendererError> SceneRenderer::ShadowPass::RenderSpotlights(const Resources& Resources, Statistics& Statistics)
	{
		if (!Resources.Snapshot || Resources.Snapshot->Meshes.empty() || !Resources.Targets.HasSpotlightMaps())
			return RenderPointLights(Resources, Statistics);

		for (size_t Slot = 0; Slot < Resources.LocalLighting.SpotlightSlotOwners().size(); ++Slot)
		{
			if (!Resources.LocalLighting.SpotlightSlotOwners()[Slot]) continue;
			const GraphicsResult Begin = Resources.Runtime.BeginRenderTarget(*Resources.Targets.SpotlightTarget(Slot));
			if (!Begin) return std::unexpected(MakeShadowDrawError("Could not begin spotlight shadow target: " + Begin.error().Message));
			RenderTargetFrameScope TargetScope(Resources.Runtime);
			for (const SceneMeshInstance& Instance : Resources.Snapshot->Meshes)
			{
				auto Material = Resources.MaterialBindings.ResolveMaterialDescription(Instance);
				if (!Material) return std::unexpected(std::move(Material.error()));
				if ((*Material)->AlphaMode == MaterialAlphaMode::Blend) continue;
				auto MaterialBinding = Resources.MaterialBindings.ResolveMaterialBinding(Instance, Resources.BindingEnvironmentAsset);
				if (!MaterialBinding) return std::unexpected(std::move(MaterialBinding.error()));
				if (auto PipelineResult = Resources.Pipelines.EnsureShadowPipeline(1.0f, (*Material)->DoubleSided); !PipelineResult)
					return std::unexpected(std::move(PipelineResult.error()));
				const GraphicsPipeline* Pipeline = Resources.Pipelines.FindShadowPipeline(1, (*Material)->DoubleSided);
				if (!Pipeline) return std::unexpected(MakeShadowResourceError("Spotlight alpha-tested shadow pipeline was not prepared"));
				auto Mesh = Resources.Meshes.GetOrLoad(Instance.MeshAsset);
				if (!Mesh)
					return std::unexpected(MakeShadowDrawError("Spotlight shadow caster mesh became unavailable: " + Mesh.error().Message));
				const glm::mat4 ShadowTransform = Resources.LocalLighting.SpotlightMatrices()[Slot] * Instance.WorldTransform;
				const auto Update =
					Resources.Runtime.WriteBuffer(*Resources.ShadowObjectConstants, 0, std::as_bytes(std::span(&ShadowTransform, 1)));
				if (!Update)
					return std::unexpected(
						MakeShadowDrawError("Could not update spotlight shadow object constants: " + Update.error().Message));
				const std::array<const BindingSet*, 2> Bindings = { *MaterialBinding, Resources.ShadowObjectBindings.get() };
				const DrawIndexedArguments Arguments{ Mesh->get().GetIndexCount(), 1, 0, 0 };
				const GraphicsResult Draw = Resources.Runtime.DrawIndexed(*Pipeline, Mesh->get(), Arguments, Bindings);
				if (!Draw) return std::unexpected(MakeShadowDrawError("Could not draw spotlight shadow caster: " + Draw.error().Message));
				++Statistics.SpotlightCasterDrawCount;
			}
			const GraphicsResult End = TargetScope.End();
			if (!End) return std::unexpected(MakeShadowDrawError("Could not end spotlight shadow target: " + End.error().Message));
			++Statistics.SpotlightPassCount;
		}
		return RenderPointLights(Resources, Statistics);
	}

	std::expected<void, SceneRendererError> SceneRenderer::ShadowPass::RenderPointLights(const Resources& Resources, Statistics& Statistics)
	{
		const auto SelectedLights = Resources.LocalLighting.SelectedLights();
		if (!Resources.Snapshot || Resources.Snapshot->Meshes.empty() || !Resources.Targets.HasPointLightCubemaps()) return {};
		if (!Resources.Pipelines.FindPointShadowPipeline(false) && !Resources.Pipelines.FindPointShadowPipeline(true))
			return std::unexpected(MakeShadowResourceError("Point-shadow pipeline was not prepared for active shadow owners"));

		for (size_t Slot = 0; Slot < Resources.LocalLighting.PointSlotOwners().size(); ++Slot)
		{
			if (!Resources.LocalLighting.PointSlotOwners()[Slot]) continue;
			const auto Selected = std::find_if(SelectedLights.begin(), SelectedLights.end(),
				[&](const SceneRenderer::LocalLightingState::SelectedLight& Light)
				{
					return Light.Point && MatchesLocalLightIdentity(
											  Light.Relevance, *Resources.LocalLighting.PointSlotOwners()[Slot], PointLocalLightTypeOrder);
				});
			if (Selected == SelectedLights.end() || !Selected->Point)
				return std::unexpected(MakeShadowDrawError("Point-shadow rendering found a stale slot owner"));

			for (size_t Face = 0; Face < 6; ++Face)
			{
				const RenderTarget& Target = Resources.Targets.PointLightCubemap(Slot)->GetFaceTarget(static_cast<uint32_t>(Face));
				RenderTargetClearValue ClearValue;
				ClearValue.Depth = 1.0f;
				const GraphicsResult Begin = Resources.Runtime.BeginRenderTarget(Target, ClearValue);
				if (!Begin)
					return std::unexpected(MakeShadowDrawError("Could not begin point-shadow cubemap face: " + Begin.error().Message));
				RenderTargetFrameScope TargetScope(Resources.Runtime);
				for (const SceneMeshInstance& Instance : Resources.Snapshot->Meshes)
				{
					auto Material = Resources.MaterialBindings.ResolveMaterialDescription(Instance);
					if (!Material) return std::unexpected(std::move(Material.error()));
					if ((*Material)->AlphaMode == MaterialAlphaMode::Blend) continue;
					auto MaterialBinding = Resources.MaterialBindings.ResolveMaterialBinding(Instance, Resources.BindingEnvironmentAsset);
					if (!MaterialBinding) return std::unexpected(std::move(MaterialBinding.error()));
					const GraphicsPipeline* Pipeline = Resources.Pipelines.FindPointShadowPipeline((*Material)->DoubleSided);
					if (!Pipeline) return std::unexpected(MakeShadowResourceError("Point-shadow alpha-tested pipeline was not prepared"));
					auto Mesh = Resources.Meshes.GetOrLoad(Instance.MeshAsset);
					if (!Mesh)
						return std::unexpected(MakeShadowDrawError("Point-shadow caster mesh became unavailable: " + Mesh.error().Message));
					const PointShadowObjectConstants Object{ Resources.LocalLighting.PointMatrices()[Slot][Face],
						glm::vec4(Selected->Point->WorldPosition, Selected->Point->Range), Instance.WorldTransform };
					const auto Update =
						Resources.Runtime.WriteBuffer(*Resources.PointShadowObjectConstants, 0, std::as_bytes(std::span(&Object, 1)));
					if (!Update)
						return std::unexpected(MakeShadowDrawError("Could not update point-shadow constants: " + Update.error().Message));
					const std::array<const BindingSet*, 2> Bindings = { *MaterialBinding, Resources.PointShadowObjectBindings.get() };
					const DrawIndexedArguments Arguments{ Mesh->get().GetIndexCount(), 1, 0, 0 };
					const GraphicsResult Draw = Resources.Runtime.DrawIndexed(*Pipeline, Mesh->get(), Arguments, Bindings);
					if (!Draw) return std::unexpected(MakeShadowDrawError("Could not draw point-shadow caster: " + Draw.error().Message));
					++Statistics.PointCasterDrawCount;
				}
				const GraphicsResult End = TargetScope.End();
				if (!End) return std::unexpected(MakeShadowDrawError("Could not end point-shadow cubemap face: " + End.error().Message));
				++Statistics.PointPassCount;
			}
		}
		return {};
	}
} // namespace PulseForge
