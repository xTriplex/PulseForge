#include "Renderer/SceneRendererPipelineCache.h"

#include "Core/Diagnostics.h"
#include "Core/Log.h"

#include <algorithm>
#include <cmath>

namespace PulseForge
{
	SceneRendererPipelineCache::SceneRendererPipelineCache(RenderDevice& Runtime) noexcept : m_Runtime(Runtime)
	{
	}

	std::expected<void, SceneRendererError> SceneRendererPipelineCache::Initialize(Resources PipelineResources)
	{
		if (m_Resources) return std::unexpected(MakeResourceError("Scene pipeline cache was initialized more than once"));
		if (!PipelineResources.SceneVertexShader || !PipelineResources.SceneFragmentShader || !PipelineResources.BackgroundVertexShader ||
			!PipelineResources.BackgroundFragmentShader || !PipelineResources.ShadowVertexShader ||
			!PipelineResources.ShadowFragmentShader || !PipelineResources.PointShadowVertexShader ||
			!PipelineResources.PointShadowFragmentShader || !PipelineResources.SceneMaterialLayout ||
			!PipelineResources.DirectionalShadowLayout || !PipelineResources.AmbientOcclusionLayout ||
			!PipelineResources.PointLightShadowLayout ||
			!PipelineResources.ShadowObjectLayout || !PipelineResources.PointShadowObjectLayout)
			return std::unexpected(MakeResourceError("Scene pipeline cache initialization received incomplete shader or layout resources"));
		m_Resources = std::move(PipelineResources);
		return {};
	}

	std::expected<void, SceneRendererError> SceneRendererPipelineCache::SetVertexLayout(const VertexLayoutDesc& Layout)
	{
		if (!m_VertexLayout)
		{
			m_VertexLayout = Layout;
			return {};
		}
		if (!HasSameVertexLayout(*m_VertexLayout, Layout))
			return std::unexpected(SceneRendererError{ SceneRendererErrorCode::UnsupportedVertexLayout, {}, {},
				"Scene meshes must use the vertex layout selected when this renderer created its pipelines" });
		return {};
	}

	std::expected<void, SceneRendererError> SceneRendererPipelineCache::EnsureScenePipeline(
		ColorTargetFormat ColorFormat, bool AlphaBlend, bool DoubleSided, WorldTransformOrientation Orientation)
	{
		if (Orientation == WorldTransformOrientation::Singular)
			return std::unexpected(MakeResourceError("Cannot create a scene pipeline for a singular mesh transform"));
		const auto Key = std::tuple{ ColorFormat, AlphaBlend, DoubleSided, Orientation };
		if (m_ScenePipelines.contains(Key)) return {};
		if (auto Initialized = RequireInitialized(); !Initialized) return Initialized;
		if (!m_VertexLayout)
			return std::unexpected(MakeResourceError("Cannot create a scene pipeline before a mesh vertex layout is prepared"));

		ScopedDiagnosticTimer Timer(m_Runtime.GetDiagnosticRecorder(), "Scene graphics pipeline creation", "SceneRendererPipelineCache");
		GraphicsPipelineDesc Description;
		Description.VertexShader = m_Resources->SceneVertexShader;
		Description.FragmentShader = m_Resources->SceneFragmentShader;
		Description.BindingLayouts = { m_Resources->SceneMaterialLayout, m_Resources->DirectionalShadowLayout,
			m_Resources->AmbientOcclusionLayout, m_Resources->PointLightShadowLayout };
		Description.VertexLayout = *m_VertexLayout;
		Description.ColorFormat = ColorFormat;
		Description.Rasterizer.Cull = DoubleSided ? CullMode::None : CullMode::Back;
		Description.Rasterizer.FrontCounterClockwise = UsesCounterClockwiseFrontFace(Orientation);
		Description.Blend.Enabled = AlphaBlend;
		Description.Depth.TestEnabled = true;
		Description.Depth.WriteEnabled = !AlphaBlend;
		Description.Depth.Compare = DepthCompareOperation::Less;
		Description.DebugName = AlphaBlend	  ? "PulseForge transparent scene pipeline"
								: DoubleSided ? "PulseForge double-sided scene pipeline"
											  : "PulseForge opaque/masked scene pipeline";
		if (Orientation == WorldTransformOrientation::Mirrored) Description.DebugName += " (mirrored transform)";
		auto Pipeline = m_Runtime.CreateGraphicsPipeline(Description);
		if (!Pipeline) return std::unexpected(MakeResourceError("Could not create scene graphics pipeline: " + Pipeline.error().Message));

		m_ScenePipelines.emplace(Key, std::move(*Pipeline));
		PF_CORE_INFO("Created scene pipeline for vertex stride {0}, color format {1}, blend {2}, double-sided {3}, mirrored {4}",
			m_VertexLayout->Stride, static_cast<uint32_t>(ColorFormat), AlphaBlend, DoubleSided,
			Orientation == WorldTransformOrientation::Mirrored);
		return {};
	}

	std::expected<void, SceneRendererError> SceneRendererPipelineCache::EnsureBackgroundPipeline(ColorTargetFormat ColorFormat)
	{
		if (m_BackgroundPipelines.contains(ColorFormat)) return {};
		if (auto Initialized = RequireInitialized(); !Initialized) return Initialized;

		GraphicsPipelineDesc Description;
		Description.VertexShader = m_Resources->BackgroundVertexShader;
		Description.FragmentShader = m_Resources->BackgroundFragmentShader;
		Description.BindingLayouts = { m_Resources->SceneMaterialLayout };
		Description.VertexLayout.Stride = sizeof(float) * 2;
		Description.VertexLayout.Attributes = { { VertexSemantic::Position, VertexFormat::Float2, 0 } };
		Description.ColorFormat = ColorFormat;
		Description.Depth.TestEnabled = false;
		Description.Depth.WriteEnabled = false;
		Description.DebugName = "PulseForge environment background pipeline";
		auto Pipeline = m_Runtime.CreateGraphicsPipeline(Description);
		if (!Pipeline)
			return std::unexpected(MakeResourceError("Could not create environment background pipeline: " + Pipeline.error().Message));
		m_BackgroundPipelines.emplace(ColorFormat, std::move(*Pipeline));
		return {};
	}

	std::expected<void, SceneRendererError> SceneRendererPipelineCache::EnsureShadowPipeline(
		float DepthBias, bool DoubleSided, WorldTransformOrientation Orientation)
	{
		if (Orientation == WorldTransformOrientation::Singular)
			return std::unexpected(MakeResourceError("Cannot create a shadow pipeline for a singular mesh transform"));
		const int32_t RasterBias = static_cast<int32_t>(std::lround(DepthBias));
		const auto Key = std::tuple{ RasterBias, DoubleSided, Orientation };
		if (m_ShadowPipelines.contains(Key)) return {};
		if (auto Initialized = RequireInitialized(); !Initialized) return Initialized;
		if (!m_VertexLayout) return std::unexpected(MakeResourceError("Cannot create a shadow pipeline before a mesh layout is prepared"));

		GraphicsPipelineDesc Description;
		Description.VertexShader = m_Resources->ShadowVertexShader;
		Description.FragmentShader = m_Resources->ShadowFragmentShader;
		Description.BindingLayouts = { m_Resources->SceneMaterialLayout, m_Resources->ShadowObjectLayout };
		auto ShadowVertexLayout = BuildAlphaTestedShadowVertexLayout(false);
		if (!ShadowVertexLayout) return std::unexpected(std::move(ShadowVertexLayout.error()));
		Description.VertexLayout = std::move(*ShadowVertexLayout);
		Description.ColorFormat = ColorTargetFormat::None;
		Description.Rasterizer.Cull = DoubleSided ? CullMode::None : CullMode::Back;
		Description.Rasterizer.FrontCounterClockwise = UsesCounterClockwiseFrontFace(Orientation);
		Description.Rasterizer.DepthBias = static_cast<float>(RasterBias);
		Description.Rasterizer.SlopeScaledDepthBias = 1.5f;
		Description.Depth.TestEnabled = true;
		Description.Depth.WriteEnabled = true;
		Description.Depth.Compare = DepthCompareOperation::Less;
		Description.DebugName =
			DoubleSided ? "PulseForge double-sided alpha-tested shadow depth pipeline" : "PulseForge alpha-tested shadow depth pipeline";
		if (Orientation == WorldTransformOrientation::Mirrored) Description.DebugName += " (mirrored transform)";
		auto Pipeline = m_Runtime.CreateGraphicsPipeline(Description);
		if (!Pipeline)
			return std::unexpected(MakeResourceError("Could not create directional shadow pipeline: " + Pipeline.error().Message));
		m_ShadowPipelines.emplace(Key, std::move(*Pipeline));
		return {};
	}

	std::expected<void, SceneRendererError> SceneRendererPipelineCache::EnsurePointShadowPipeline(
		bool DoubleSided, WorldTransformOrientation Orientation)
	{
		if (Orientation == WorldTransformOrientation::Singular)
			return std::unexpected(MakeResourceError("Cannot create a point-shadow pipeline for a singular mesh transform"));
		const bool Mirrored = Orientation == WorldTransformOrientation::Mirrored;
		const size_t Index = (DoubleSided ? 2u : 0u) + (Mirrored ? 1u : 0u);
		if (m_PointShadowPipelines[Index]) return {};
		if (auto Initialized = RequireInitialized(); !Initialized) return Initialized;
		if (!m_VertexLayout)
			return std::unexpected(MakeResourceError("Cannot create point-shadow pipeline before a mesh layout is prepared"));

		auto ShadowVertexLayout = BuildAlphaTestedShadowVertexLayout(true);
		if (!ShadowVertexLayout) return std::unexpected(std::move(ShadowVertexLayout.error()));
		GraphicsPipelineDesc Description;
		Description.VertexShader = m_Resources->PointShadowVertexShader;
		Description.FragmentShader = m_Resources->PointShadowFragmentShader;
		Description.BindingLayouts = { m_Resources->SceneMaterialLayout, m_Resources->PointShadowObjectLayout };
		Description.VertexLayout = std::move(*ShadowVertexLayout);
		Description.ColorFormat = ColorTargetFormat::None;
		Description.Rasterizer.Cull = DoubleSided ? CullMode::None : CullMode::Back;
		Description.Rasterizer.FrontCounterClockwise = UsesCounterClockwiseFrontFace(Orientation);
		Description.Depth.TestEnabled = true;
		Description.Depth.WriteEnabled = true;
		Description.Depth.Compare = DepthCompareOperation::Less;
		Description.DebugName =
			DoubleSided ? "PulseForge double-sided alpha-tested point-shadow pipeline" : "PulseForge alpha-tested point-shadow pipeline";
		if (Mirrored) Description.DebugName += " (mirrored transform)";
		auto Pipeline = m_Runtime.CreateGraphicsPipeline(Description);
		if (!Pipeline) return std::unexpected(MakeResourceError("Could not create point-shadow pipeline: " + Pipeline.error().Message));
		m_PointShadowPipelines[Index] = std::move(*Pipeline);
		return {};
	}

	const GraphicsPipeline* SceneRendererPipelineCache::FindScenePipeline(
		ColorTargetFormat ColorFormat, bool AlphaBlend, bool DoubleSided, WorldTransformOrientation Orientation) const noexcept
	{
		if (Orientation == WorldTransformOrientation::Singular) return nullptr;
		const auto Found = m_ScenePipelines.find(std::tuple{ ColorFormat, AlphaBlend, DoubleSided, Orientation });
		return Found != m_ScenePipelines.end() ? Found->second.get() : nullptr;
	}

	const GraphicsPipeline* SceneRendererPipelineCache::FindBackgroundPipeline(ColorTargetFormat ColorFormat) const noexcept
	{
		const auto Found = m_BackgroundPipelines.find(ColorFormat);
		return Found != m_BackgroundPipelines.end() ? Found->second.get() : nullptr;
	}

	const GraphicsPipeline* SceneRendererPipelineCache::FindShadowPipeline(
		int32_t RasterBias, bool DoubleSided, WorldTransformOrientation Orientation) const noexcept
	{
		if (Orientation == WorldTransformOrientation::Singular) return nullptr;
		const auto Found = m_ShadowPipelines.find({ RasterBias, DoubleSided, Orientation });
		return Found != m_ShadowPipelines.end() ? Found->second.get() : nullptr;
	}

	const GraphicsPipeline* SceneRendererPipelineCache::FindPointShadowPipeline(
		bool DoubleSided, WorldTransformOrientation Orientation) const noexcept
	{
		if (Orientation == WorldTransformOrientation::Singular) return nullptr;
		const bool Mirrored = Orientation == WorldTransformOrientation::Mirrored;
		return m_PointShadowPipelines[(DoubleSided ? 2u : 0u) + (Mirrored ? 1u : 0u)].get();
	}

	bool SceneRendererPipelineCache::HasSameVertexLayout(const VertexLayoutDesc& Left, const VertexLayoutDesc& Right) noexcept
	{
		if (Left.Stride != Right.Stride || Left.Attributes.size() != Right.Attributes.size()) return false;
		for (size_t Index = 0; Index < Left.Attributes.size(); ++Index)
		{
			const VertexAttributeDesc& LeftAttribute = Left.Attributes[Index];
			const VertexAttributeDesc& RightAttribute = Right.Attributes[Index];
			if (LeftAttribute.Semantic != RightAttribute.Semantic || LeftAttribute.Format != RightAttribute.Format ||
				LeftAttribute.Offset != RightAttribute.Offset)
				return false;
		}
		return true;
	}

	std::expected<VertexLayoutDesc, SceneRendererError> SceneRendererPipelineCache::BuildAlphaTestedShadowVertexLayout(
		bool PointLightShadow) const
	{
		VertexLayoutDesc Result;
		Result.Stride = m_VertexLayout->Stride;
		const auto AppendAttribute = [this, &Result, PointLightShadow](VertexSemantic Semantic) -> std::expected<void, SceneRendererError>
		{
			const auto Attribute = std::find_if(m_VertexLayout->Attributes.begin(), m_VertexLayout->Attributes.end(),
				[Semantic](const VertexAttributeDesc& Candidate) { return Candidate.Semantic == Semantic; });
			if (Attribute != m_VertexLayout->Attributes.end())
			{
				Result.Attributes.push_back(*Attribute);
				return {};
			}

			std::string Message;
			if (Semantic == VertexSemantic::Position)
				Message = PointLightShadow ? "The mesh layout has no position attribute for point-shadow rendering"
										   : "The mesh layout has no position attribute for shadow rendering";
			else if (Semantic == VertexSemantic::Color)
				Message = PointLightShadow ? "The mesh layout has no color attribute for point-shadow alpha tests"
										   : "The mesh layout has no color attribute for alpha-tested shadows";
			else
				Message = PointLightShadow ? "The mesh layout has no texture coordinates for point-shadow alpha tests"
										   : "The mesh layout has no texture coordinates for alpha-tested shadows";
			return std::unexpected(MakeResourceError(std::move(Message)));
		};
		if (auto Position = AppendAttribute(VertexSemantic::Position); !Position) return std::unexpected(std::move(Position.error()));
		if (auto Color = AppendAttribute(VertexSemantic::Color); !Color) return std::unexpected(std::move(Color.error()));
		if (auto TexCoord = AppendAttribute(VertexSemantic::TexCoord); !TexCoord) return std::unexpected(std::move(TexCoord.error()));
		return Result;
	}

	std::expected<void, SceneRendererError> SceneRendererPipelineCache::RequireInitialized() const
	{
		if (m_Resources) return {};
		return std::unexpected(MakeResourceError("Scene pipeline cache was used before initialization"));
	}

	SceneRendererError SceneRendererPipelineCache::MakeResourceError(std::string Message)
	{
		return { SceneRendererErrorCode::ResourceCreationFailed, {}, {}, std::move(Message) };
	}
} // namespace PulseForge
