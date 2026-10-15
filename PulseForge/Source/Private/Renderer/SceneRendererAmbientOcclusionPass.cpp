#include "Core/PulseForgePCH.h"
#include "Renderer/SceneRendererAmbientOcclusionPass.h"

#include "Core/Application.h"
#include "Core/Log.h"
#include "Core/ScopedProfileTimer.h"
#include "Assets/MaterialAsset.h"
#include "Renderer/AssetCache/MeshAssetCache.h"
#include "Renderer/Mesh.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <span>

#include <glm/gtc/matrix_inverse.hpp>

namespace PulseForge
{
	namespace
	{
		SceneRendererError MakeAmbientOcclusionResourceError(std::string Message)
		{
			return { SceneRendererErrorCode::ResourceCreationFailed, {}, {}, std::move(Message) };
		}

		SceneRendererError MakeAmbientOcclusionDrawError(std::string Message)
		{
			return { SceneRendererErrorCode::DrawFailed, {}, {}, std::move(Message) };
		}

		bool IsFiniteMatrix(const glm::mat4& Matrix)
		{
			for (int Column = 0; Column < 4; ++Column)
				for (int Row = 0; Row < 4; ++Row)
					if (!std::isfinite(Matrix[Column][Row]))
						return false;
			return true;
		}

		class RenderTargetScope final
		{
		public:
			explicit RenderTargetScope(Application& Runtime) noexcept : m_Runtime(Runtime) {}
			RenderTargetScope(const RenderTargetScope&) = delete;
			RenderTargetScope& operator=(const RenderTargetScope&) = delete;

			~RenderTargetScope() noexcept
			{
				if (!m_Active)
					return;
				try
				{
					const GraphicsResult EndResult = m_Runtime.EndRenderTarget();
					if (!EndResult)
						PF_CORE_ERROR("Could not close render target during SSAO unwinding: {0}", EndResult.error().Message);
				}
				catch (const std::exception& Exception)
				{
					PF_CORE_ERROR("Exception while closing render target during SSAO unwinding: {0}", Exception.what());
				}
				catch (...)
				{
					PF_CORE_ERROR("Unknown exception while closing render target during SSAO unwinding");
				}
			}

			[[nodiscard]] GraphicsResult End()
			{
				m_Active = false;
				return m_Runtime.EndRenderTarget();
			}

		private:
			Application& m_Runtime;
			bool m_Active = true;
		};
	}

	SceneRenderer::AmbientOcclusionPass::AmbientOcclusionPass(Application& Runtime) noexcept
		: m_Runtime(Runtime)
	{
	}

	std::expected<void, SceneRendererError> SceneRenderer::AmbientOcclusionPass::Initialize(
		ShaderHandle PrepassVertex,
		ShaderHandle PrepassFragment,
		ShaderHandle EvaluationVertex,
		ShaderHandle EvaluationFragment,
		ShaderHandle BlurVertex,
		ShaderHandle BlurFragment)
	{
		m_PrepassVertexShader = std::move(PrepassVertex);
		m_PrepassFragmentShader = std::move(PrepassFragment);
		m_EvaluationVertexShader = std::move(EvaluationVertex);
		m_EvaluationFragmentShader = std::move(EvaluationFragment);
		m_BlurVertexShader = std::move(BlurVertex);
		m_BlurFragmentShader = std::move(BlurFragment);

		SamplerDesc PointDescription;
		PointDescription.Minification = SamplerFilter::Nearest;
		PointDescription.Magnification = SamplerFilter::Nearest;
		PointDescription.AddressU = SamplerAddressMode::ClampToEdge;
		PointDescription.AddressV = SamplerAddressMode::ClampToEdge;
		PointDescription.DebugName = "PulseForge SSAO point clamp sampler";
		auto PointSampler = m_Runtime.CreateSampler(PointDescription);
		if (!PointSampler)
			return std::unexpected(MakeAmbientOcclusionResourceError(
				"Could not create SSAO point sampler: " + PointSampler.error().Message));
		m_PointSampler = std::move(*PointSampler);

		SamplerDesc LinearDescription = PointDescription;
		LinearDescription.Minification = SamplerFilter::Linear;
		LinearDescription.Magnification = SamplerFilter::Linear;
		LinearDescription.DebugName = "PulseForge SSAO linear clamp sampler";
		auto LinearSampler = m_Runtime.CreateSampler(LinearDescription);
		if (!LinearSampler)
			return std::unexpected(MakeAmbientOcclusionResourceError(
				"Could not create SSAO linear sampler: " + LinearSampler.error().Message));
		m_LinearSampler = std::move(*LinearSampler);

		constexpr std::array<uint8_t, 4> UnoccludedPixel{ 255, 255, 255, 255 };
		TextureDesc FallbackDescription;
		FallbackDescription.Width = 1;
		FallbackDescription.Height = 1;
		FallbackDescription.Format = TextureFormat::RGBA8_UNorm;
		FallbackDescription.DebugName = "PulseForge unoccluded SSAO fallback";
		auto FallbackTexture = m_Runtime.CreateTexture(
			FallbackDescription, std::as_bytes(std::span<const uint8_t>(UnoccludedPixel)));
		if (!FallbackTexture)
			return std::unexpected(MakeAmbientOcclusionResourceError(
				"Could not create the unoccluded SSAO fallback: " + FallbackTexture.error().Message));
		m_FallbackOcclusionTexture = std::move(*FallbackTexture);

		BindingLayoutDesc FinalDescription;
		FinalDescription.Visibility = ShaderVisibility::Fragment;
		FinalDescription.ShaderRegisterSpace = 2;
		FinalDescription.Items = { { BindingResourceType::Texture2D, 0 }, { BindingResourceType::Sampler, 0 } };
		FinalDescription.DebugName = "PulseForge SSAO final result (space2)";
		auto FinalLayout = m_Runtime.CreateBindingLayout(FinalDescription);
		if (!FinalLayout)
			return std::unexpected(MakeAmbientOcclusionResourceError(
				"Could not create SSAO final layout: " + FinalLayout.error().Message));
		m_FinalLayout = std::move(*FinalLayout);

		BindingLayoutDesc EvaluationDescription;
		EvaluationDescription.Visibility = ShaderVisibility::Fragment;
		EvaluationDescription.ShaderRegisterSpace = 4;
		EvaluationDescription.Items = {
			{ BindingResourceType::Texture2D, 0 }, { BindingResourceType::Texture2D, 1 },
			{ BindingResourceType::Sampler, 0 }, { BindingResourceType::ConstantBuffer, 0 }
		};
		EvaluationDescription.DebugName = "PulseForge SSAO evaluation resources (space4)";
		auto EvaluationLayout = m_Runtime.CreateBindingLayout(EvaluationDescription);
		if (!EvaluationLayout)
			return std::unexpected(MakeAmbientOcclusionResourceError(
				"Could not create SSAO evaluation layout: " + EvaluationLayout.error().Message));
		m_EvaluationLayout = std::move(*EvaluationLayout);

		BindingLayoutDesc BlurDescription;
		BlurDescription.Visibility = ShaderVisibility::Fragment;
		BlurDescription.ShaderRegisterSpace = 4;
		BlurDescription.Items = {
			{ BindingResourceType::Texture2D, 0 }, { BindingResourceType::Texture2D, 1 },
			{ BindingResourceType::Texture2D, 2 }, { BindingResourceType::Sampler, 0 },
			{ BindingResourceType::Sampler, 1 }, { BindingResourceType::ConstantBuffer, 0 }
		};
		BlurDescription.DebugName = "PulseForge SSAO bilateral blur resources (space4)";
		auto BlurLayout = m_Runtime.CreateBindingLayout(BlurDescription);
		if (!BlurLayout)
			return std::unexpected(MakeAmbientOcclusionResourceError(
				"Could not create SSAO blur layout: " + BlurLayout.error().Message));
		m_BlurLayout = std::move(*BlurLayout);

		BindingLayoutDesc PrepassDescription;
		PrepassDescription.Visibility = ShaderVisibility::AllGraphics;
		PrepassDescription.ShaderRegisterSpace = 4;
		PrepassDescription.Items = { { BindingResourceType::ConstantBuffer, 0 } };
		PrepassDescription.DebugName = "PulseForge SSAO prepass constants (space4)";
		auto PrepassLayout = m_Runtime.CreateBindingLayout(PrepassDescription);
		if (!PrepassLayout)
			return std::unexpected(MakeAmbientOcclusionResourceError(
				"Could not create SSAO prepass layout: " + PrepassLayout.error().Message));
		m_PrepassLayout = std::move(*PrepassLayout);

		BindingSetDesc FallbackBindingsDescription;
		FallbackBindingsDescription.Layout = m_FinalLayout;
		FallbackBindingsDescription.Textures.push_back({ 0, std::cref(*m_FallbackOcclusionTexture) });
		FallbackBindingsDescription.Samplers.push_back({ 0, std::cref(*m_LinearSampler) });
		auto FallbackBindings = m_Runtime.CreateBindingSet(FallbackBindingsDescription);
		if (!FallbackBindings)
			return std::unexpected(MakeAmbientOcclusionResourceError(
				"Could not create fallback SSAO bindings: " + FallbackBindings.error().Message));
		m_FallbackBindings = std::move(*FallbackBindings);

		m_Kernel = GenerateAmbientOcclusionKernel();
		Constants InitialParameters;
		InitialParameters.Parameters = glm::vec4(m_Settings.Radius, m_Settings.Bias, m_Settings.Strength, 0.0f);
		InitialParameters.Kernel = m_Kernel;
		BufferDesc ParametersDescription;
		ParametersDescription.ByteSize = sizeof(InitialParameters);
		ParametersDescription.Usage = BufferUsage::Constant;
		ParametersDescription.DebugName = "PulseForge SSAO kernel and parameters";
		auto Parameters = m_Runtime.CreateBuffer(
			ParametersDescription, std::as_bytes(std::span(&InitialParameters, 1)));
		if (!Parameters)
			return std::unexpected(MakeAmbientOcclusionResourceError(
				"Could not create SSAO parameters: " + Parameters.error().Message));
		m_ParametersBuffer = std::move(*Parameters);

		const BlurConstants InitialBlurParameters{};
		BufferDesc BlurParametersDescription;
		BlurParametersDescription.ByteSize = sizeof(InitialBlurParameters);
		BlurParametersDescription.Usage = BufferUsage::Constant;
		BlurParametersDescription.DebugName = "PulseForge SSAO bilateral blur parameters";
		auto BlurParameters = m_Runtime.CreateBuffer(
			BlurParametersDescription, std::as_bytes(std::span(&InitialBlurParameters, 1)));
		if (!BlurParameters)
			return std::unexpected(MakeAmbientOcclusionResourceError(
				"Could not create SSAO blur parameters: " + BlurParameters.error().Message));
		m_BlurParametersBuffer = std::move(*BlurParameters);

		const ObjectConstants InitialObjectConstants{};
		BufferDesc ObjectDescription;
		ObjectDescription.ByteSize = sizeof(InitialObjectConstants);
		ObjectDescription.Usage = BufferUsage::Constant;
		ObjectDescription.DebugName = "PulseForge SSAO prepass object constants";
		auto ObjectBuffer = m_Runtime.CreateBuffer(
			ObjectDescription, std::as_bytes(std::span(&InitialObjectConstants, 1)));
		if (!ObjectBuffer)
			return std::unexpected(MakeAmbientOcclusionResourceError(
				"Could not create SSAO prepass constants: " + ObjectBuffer.error().Message));
		m_ObjectConstantsBuffer = std::move(*ObjectBuffer);
		BindingSetDesc PrepassBindingsDescription;
		PrepassBindingsDescription.Layout = m_PrepassLayout;
		PrepassBindingsDescription.Buffers.push_back({ 0, std::cref(*m_ObjectConstantsBuffer) });
		auto PrepassBindings = m_Runtime.CreateBindingSet(PrepassBindingsDescription);
		if (!PrepassBindings)
			return std::unexpected(MakeAmbientOcclusionResourceError(
				"Could not create SSAO prepass object bindings: " + PrepassBindings.error().Message));
		m_PrepassBindings = std::move(*PrepassBindings);
		return {};
	}

	std::expected<void, SceneRendererError> SceneRenderer::AmbientOcclusionPass::EnsureResources(
		uint32_t Width,
		uint32_t Height)
	{
		const auto [HalfWidth, HalfHeight] = CalculateAmbientOcclusionExtent(Width, Height);
		if (m_Targets[0] && m_Width == Width && m_Height == Height)
			return {};
		Detail::ScopedProfileTimer Timer("SSAO first/resize resource creation");

		const auto CreateTarget = [this](uint32_t TargetWidth, uint32_t TargetHeight,
			DepthAttachmentMode DepthMode, const char* Name) -> std::expected<RenderTargetHandle, SceneRendererError>
		{
			RenderTargetDesc Description;
			Description.Width = TargetWidth;
			Description.Height = TargetHeight;
			Description.ColorFormat = ColorTargetFormat::RGBA8_UNorm;
			Description.DepthMode = DepthMode;
			Description.DebugName = Name;
			auto Target = m_Runtime.CreateRenderTarget(Description);
			if (!Target)
				return std::unexpected(MakeAmbientOcclusionResourceError(
					std::string("Could not create ") + Name + ": " + Target.error().Message));
			return std::move(*Target);
		};

		auto Prepass = CreateTarget(Width, Height, DepthAttachmentMode::ShaderReadableAttachment,
			"PulseForge SSAO camera normal-depth target");
		auto Raw = CreateTarget(HalfWidth, HalfHeight, DepthAttachmentMode::None,
			"PulseForge raw half-resolution SSAO target");
		auto Horizontal = CreateTarget(HalfWidth, HalfHeight, DepthAttachmentMode::None,
			"PulseForge horizontal SSAO blur target");
		auto Vertical = CreateTarget(HalfWidth, HalfHeight, DepthAttachmentMode::None,
			"PulseForge vertical SSAO blur target");
		if (!Prepass || !Raw || !Horizontal || !Vertical)
			return std::unexpected(MakeAmbientOcclusionResourceError("Could not allocate the persistent SSAO target set"));

		const Texture* PrepassNormal = (*Prepass)->GetColorTexture();
		const Texture* PrepassDepth = (*Prepass)->GetDepthTexture();
		const Texture* RawTexture = (*Raw)->GetColorTexture();
		const Texture* HorizontalTexture = (*Horizontal)->GetColorTexture();
		const Texture* VerticalTexture = (*Vertical)->GetColorTexture();
		if (!PrepassNormal || !PrepassDepth || !RawTexture || !HorizontalTexture || !VerticalTexture)
			return std::unexpected(MakeAmbientOcclusionResourceError(
				"SSAO target allocation returned a missing required attachment"));

		BindingSetDesc EvaluationDescription;
		EvaluationDescription.Layout = m_EvaluationLayout;
		EvaluationDescription.Textures = { { 0, std::cref(*PrepassDepth) }, { 1, std::cref(*PrepassNormal) } };
		EvaluationDescription.Samplers = { { 0, std::cref(*m_PointSampler) } };
		EvaluationDescription.Buffers = { { 0, std::cref(*m_ParametersBuffer) } };
		auto EvaluationBindings = m_Runtime.CreateBindingSet(EvaluationDescription);
		if (!EvaluationBindings)
			return std::unexpected(MakeAmbientOcclusionResourceError(
				"Could not create SSAO evaluation bindings: " + EvaluationBindings.error().Message));

		const auto CreateBlurBindings = [this, PrepassNormal, PrepassDepth](const Texture& AoTexture)
			-> std::expected<BindingSetHandle, SceneRendererError>
		{
			BindingSetDesc Description;
			Description.Layout = m_BlurLayout;
			Description.Textures = {
				{ 0, std::cref(AoTexture) }, { 1, std::cref(*PrepassNormal) }, { 2, std::cref(*PrepassDepth) }
			};
			Description.Samplers = {
				{ 0, std::cref(*m_LinearSampler) }, { 1, std::cref(*m_PointSampler) }
			};
			Description.Buffers = { { 0, std::cref(*m_BlurParametersBuffer) } };
			auto Bindings = m_Runtime.CreateBindingSet(Description);
			if (!Bindings)
				return std::unexpected(MakeAmbientOcclusionResourceError(
					"Could not create SSAO bilateral bindings: " + Bindings.error().Message));
			return std::move(*Bindings);
		};
		auto HorizontalBindings = CreateBlurBindings(*RawTexture);
		auto VerticalBindings = CreateBlurBindings(*HorizontalTexture);
		if (!HorizontalBindings || !VerticalBindings)
			return std::unexpected(MakeAmbientOcclusionResourceError("Could not create the SSAO blur binding sets"));

		BindingSetDesc FinalDescription;
		FinalDescription.Layout = m_FinalLayout;
		FinalDescription.Textures.push_back({ 0, std::cref(*VerticalTexture) });
		FinalDescription.Samplers.push_back({ 0, std::cref(*m_LinearSampler) });
		auto FinalBindings = m_Runtime.CreateBindingSet(FinalDescription);
		if (!FinalBindings)
			return std::unexpected(MakeAmbientOcclusionResourceError(
				"Could not create final SSAO bindings: " + FinalBindings.error().Message));

		m_Targets = { std::move(*Prepass), std::move(*Raw), std::move(*Horizontal), std::move(*Vertical) };
		m_EvaluationBindings = std::move(*EvaluationBindings);
		m_BlurBindings = { std::move(*HorizontalBindings), std::move(*VerticalBindings) };
		m_FinalBindings = std::move(*FinalBindings);
		m_Width = Width;
		m_Height = Height;
		return {};
	}

	std::expected<void, SceneRendererError> SceneRenderer::AmbientOcclusionPass::EnsurePipelines(
		const BindingLayoutHandle& SceneMaterialLayout,
		const std::optional<VertexLayoutDesc>& SceneVertexLayout)
	{
		if (!m_EvaluationPipeline)
		{
			GraphicsPipelineDesc Description;
			Description.VertexShader = m_EvaluationVertexShader;
			Description.FragmentShader = m_EvaluationFragmentShader;
			Description.BindingLayouts = { m_EvaluationLayout };
			Description.VertexLayout.Stride = sizeof(float) * 2;
			Description.VertexLayout.Attributes = { { VertexSemantic::Position, VertexFormat::Float2, 0 } };
			Description.ColorFormat = ColorTargetFormat::RGBA8_UNorm;
			Description.DepthAttachmentEnabled = false;
			Description.DebugName = "PulseForge half-resolution SSAO evaluation pipeline";
			auto Pipeline = m_Runtime.CreateGraphicsPipeline(Description);
			if (!Pipeline)
				return std::unexpected(MakeAmbientOcclusionResourceError(
					"Could not create SSAO evaluation pipeline: " + Pipeline.error().Message));
			m_EvaluationPipeline = std::move(*Pipeline);
		}

		if (!m_BlurPipeline)
		{
			GraphicsPipelineDesc Description;
			Description.VertexShader = m_BlurVertexShader;
			Description.FragmentShader = m_BlurFragmentShader;
			Description.BindingLayouts = { m_BlurLayout };
			Description.VertexLayout.Stride = sizeof(float) * 2;
			Description.VertexLayout.Attributes = { { VertexSemantic::Position, VertexFormat::Float2, 0 } };
			Description.ColorFormat = ColorTargetFormat::RGBA8_UNorm;
			Description.DepthAttachmentEnabled = false;
			Description.DebugName = "PulseForge bilateral SSAO blur pipeline";
			auto Pipeline = m_Runtime.CreateGraphicsPipeline(Description);
			if (!Pipeline)
				return std::unexpected(MakeAmbientOcclusionResourceError(
					"Could not create SSAO blur pipeline: " + Pipeline.error().Message));
			m_BlurPipeline = std::move(*Pipeline);
		}

		if (!SceneVertexLayout)
			return {};
		for (size_t Index = 0; Index < m_PrepassPipelines.size(); ++Index)
		{
			if (m_PrepassPipelines[Index])
				continue;
			GraphicsPipelineDesc Description;
			Description.VertexShader = m_PrepassVertexShader;
			Description.FragmentShader = m_PrepassFragmentShader;
			Description.BindingLayouts = { SceneMaterialLayout, m_PrepassLayout };
			Description.VertexLayout.Stride = SceneVertexLayout->Stride;
			for (const VertexSemantic Semantic : {
				VertexSemantic::Position, VertexSemantic::Color, VertexSemantic::TexCoord, VertexSemantic::Normal })
			{
				const auto Attribute = std::find_if(SceneVertexLayout->Attributes.begin(), SceneVertexLayout->Attributes.end(),
					[Semantic](const VertexAttributeDesc& Candidate) { return Candidate.Semantic == Semantic; });
				if (Attribute == SceneVertexLayout->Attributes.end())
					return std::unexpected(MakeAmbientOcclusionResourceError(
						"Mesh layout lacks position, texture coordinates, or normals required by the SSAO alpha prepass"));
				Description.VertexLayout.Attributes.push_back(*Attribute);
			}
			Description.ColorFormat = ColorTargetFormat::RGBA8_UNorm;
			Description.Depth.TestEnabled = true;
			Description.Depth.WriteEnabled = true;
			Description.Depth.Compare = DepthCompareOperation::Less;
			Description.DepthAttachmentEnabled = true;
			Description.Rasterizer.Cull = Index == 0 ? CullMode::Back : CullMode::None;
			Description.DebugName = Index == 0
				? "PulseForge SSAO single-sided alpha prepass"
				: "PulseForge SSAO double-sided alpha prepass";
			auto Pipeline = m_Runtime.CreateGraphicsPipeline(Description);
			if (!Pipeline)
				return std::unexpected(MakeAmbientOcclusionResourceError(
					"Could not create SSAO prepass pipeline: " + Pipeline.error().Message));
			m_PrepassPipelines[Index] = std::move(*Pipeline);
		}
		return {};
	}

	std::expected<void, SceneRendererError> SceneRenderer::AmbientOcclusionPass::Render(
		const RenderInputs& Inputs,
		uint32_t Width,
		uint32_t Height)
	{
		m_FrameAvailable = false;
		if (!m_Settings.Enabled || Inputs.Snapshot.Meshes.empty() || !Inputs.Snapshot.HasCameraFrustum ||
			Width == 0 || Height == 0)
			return {};
		if (auto Resources = EnsureResources(Width, Height); !Resources)
			return std::unexpected(std::move(Resources.error()));
		if (auto Pipelines = EnsurePipelines(Inputs.SceneMaterialLayout, Inputs.SceneVertexLayout); !Pipelines)
			return std::unexpected(std::move(Pipelines.error()));
		if (!m_PrepassPipelines[0] || !m_PrepassPipelines[1])
			return std::unexpected(MakeAmbientOcclusionResourceError(
				"SSAO normal-depth pipeline is not available for the prepared mesh layout"));

		const glm::mat4 InverseProjection = glm::inverse(Inputs.Snapshot.Projection);
		if (!IsFiniteMatrix(InverseProjection))
			return std::unexpected(MakeAmbientOcclusionDrawError("SSAO requires an invertible camera projection"));
		const auto [HalfWidth, HalfHeight] = CalculateAmbientOcclusionExtent(Width, Height);
		Constants Parameters;
		Parameters.InverseProjection = InverseProjection;
		Parameters.Projection = Inputs.Snapshot.Projection;
		Parameters.OutputSize = glm::vec4(static_cast<float>(Width), static_cast<float>(Height),
			1.0f / static_cast<float>(Width), 1.0f / static_cast<float>(Height));
		Parameters.HalfSize = glm::vec4(static_cast<float>(HalfWidth), static_cast<float>(HalfHeight),
			1.0f / static_cast<float>(HalfWidth), 1.0f / static_cast<float>(HalfHeight));
		Parameters.Parameters = glm::vec4(m_Settings.Radius, m_Settings.Bias, m_Settings.Strength, 0.0f);
		Parameters.Kernel = m_Kernel;
		if (const auto Update = m_Runtime.WriteBuffer(*m_ParametersBuffer, 0,
			std::as_bytes(std::span(&Parameters, 1))); !Update)
			return std::unexpected(MakeAmbientOcclusionDrawError(
				"Could not update SSAO parameters: " + Update.error().Message));

		RenderTargetClearValue PrepassClear;
		PrepassClear.Color = { 0.5f, 0.5f, 1.0f, 1.0f };
		if (const GraphicsResult Begin = m_Runtime.BeginRenderTarget(*m_Targets[0], PrepassClear); !Begin)
			return std::unexpected(MakeAmbientOcclusionDrawError(
				"Could not begin SSAO normal-depth prepass: " + Begin.error().Message));
		RenderTargetScope PrepassScope(m_Runtime);
		for (const SceneMeshInstance& Instance : Inputs.Snapshot.Meshes)
		{
			auto Material = Inputs.ResolveMaterial(Instance);
			if (!Material)
				return std::unexpected(std::move(Material.error()));
			if ((*Material)->AlphaMode == MaterialAlphaMode::Blend)
				continue;
			auto Bindings = Inputs.ResolveBindings(Instance);
			if (!Bindings)
				return std::unexpected(std::move(Bindings.error()));
			auto Mesh = Inputs.Meshes.GetOrLoad(Instance.MeshAsset);
			if (!Mesh)
				return std::unexpected(MakeAmbientOcclusionDrawError(
					"SSAO prepass mesh became unavailable: " + Mesh.error().Message));

			const glm::mat4 ViewModel = Inputs.Snapshot.View * Instance.WorldTransform;
			const glm::mat4 ViewNormalTransform = glm::transpose(glm::inverse(ViewModel));
			if (!IsFiniteMatrix(ViewNormalTransform))
				return std::unexpected(MakeAmbientOcclusionDrawError(
					"SSAO prepass cannot transform normals for a singular mesh transform"));
			const ObjectConstants Object{
				Inputs.Snapshot.ViewProjection * Instance.WorldTransform,
				ViewNormalTransform };
			if (const auto Update = m_Runtime.WriteBuffer(*m_ObjectConstantsBuffer, 0,
				std::as_bytes(std::span(&Object, 1))); !Update)
				return std::unexpected(MakeAmbientOcclusionDrawError(
					"Could not update SSAO object constants: " + Update.error().Message));

			const std::array<const BindingSet*, 2> BindingSets = { *Bindings, m_PrepassBindings.get() };
			const size_t PipelineIndex = (*Material)->DoubleSided ? 1u : 0u;
			const GraphicsResult Draw = m_Runtime.DrawIndexed(*m_PrepassPipelines[PipelineIndex], Mesh->get(),
				{ Mesh->get().GetIndexCount(), 1, 0, 0 }, BindingSets);
			if (!Draw)
				return std::unexpected(MakeAmbientOcclusionDrawError(
					"Could not draw SSAO normal-depth prepass: " + Draw.error().Message));
		}
		if (const GraphicsResult End = PrepassScope.End(); !End)
			return std::unexpected(MakeAmbientOcclusionDrawError(
				"Could not end SSAO normal-depth prepass: " + End.error().Message));

		const auto DrawFullscreen = [this, &Inputs](const RenderTarget& Target, const GraphicsPipeline& Pipeline,
			const BindingSet& Bindings, const char* Name) -> std::expected<void, SceneRendererError>
		{
			RenderTargetClearValue Clear;
			Clear.Color = { 1.0f, 1.0f, 1.0f, 1.0f };
			if (const GraphicsResult Begin = m_Runtime.BeginRenderTarget(Target, Clear); !Begin)
				return std::unexpected(MakeAmbientOcclusionDrawError(
					std::string("Could not begin ") + Name + ": " + Begin.error().Message));
			RenderTargetScope Scope(m_Runtime);
			const std::array<const BindingSet*, 1> BindingSets = { &Bindings };
			const GraphicsResult Draw = m_Runtime.Draw(Pipeline, Inputs.FullscreenTriangle, { 3, 1, 0, 0 }, BindingSets);
			if (!Draw)
				return std::unexpected(MakeAmbientOcclusionDrawError(
					std::string("Could not draw ") + Name + ": " + Draw.error().Message));
			if (const GraphicsResult End = Scope.End(); !End)
				return std::unexpected(MakeAmbientOcclusionDrawError(
					std::string("Could not end ") + Name + ": " + End.error().Message));
			return {};
		};

		if (auto Result = DrawFullscreen(*m_Targets[1], *m_EvaluationPipeline,
			*m_EvaluationBindings, "SSAO evaluation pass"); !Result)
			return std::unexpected(std::move(Result.error()));
		BlurConstants BlurParameters;
		BlurParameters.InverseProjection = InverseProjection;
		BlurParameters.OutputSize = Parameters.OutputSize;
		BlurParameters.HalfSize = Parameters.HalfSize;
		BlurParameters.Direction = glm::vec4(1.0f / static_cast<float>(HalfWidth), 0.0f, 0.0f, 0.0f);
		if (const auto Update = m_Runtime.WriteBuffer(*m_BlurParametersBuffer, 0,
			std::as_bytes(std::span(&BlurParameters, 1))); !Update)
			return std::unexpected(MakeAmbientOcclusionDrawError(
				"Could not update horizontal SSAO blur parameters: " + Update.error().Message));
		if (auto Result = DrawFullscreen(*m_Targets[2], *m_BlurPipeline,
			*m_BlurBindings[0], "horizontal SSAO bilateral blur"); !Result)
			return std::unexpected(std::move(Result.error()));

		BlurParameters.Direction = glm::vec4(0.0f, 1.0f / static_cast<float>(HalfHeight), 0.0f, 0.0f);
		if (const auto Update = m_Runtime.WriteBuffer(*m_BlurParametersBuffer, 0,
			std::as_bytes(std::span(&BlurParameters, 1))); !Update)
			return std::unexpected(MakeAmbientOcclusionDrawError(
				"Could not update vertical SSAO blur parameters: " + Update.error().Message));
		if (auto Result = DrawFullscreen(*m_Targets[3], *m_BlurPipeline,
			*m_BlurBindings[1], "vertical SSAO bilateral blur"); !Result)
			return std::unexpected(std::move(Result.error()));
		m_FrameAvailable = true;
		return {};
	}

	bool SceneRenderer::AmbientOcclusionPass::SetSettings(const AmbientOcclusionSettings& Settings) noexcept
	{
		if (!std::isfinite(Settings.Radius) || Settings.Radius <= 0.0f || Settings.Radius > 10.0f ||
			!std::isfinite(Settings.Bias) || Settings.Bias < 0.0f || Settings.Bias > Settings.Radius ||
			!std::isfinite(Settings.Strength) || Settings.Strength < 0.0f || Settings.Strength > 4.0f)
			return false;
		m_Settings = Settings;
		return true;
	}

	const Texture* SceneRenderer::AmbientOcclusionPass::GetFallbackOcclusionTexture() const noexcept
	{
		return m_FallbackOcclusionTexture.get();
	}

	const BindingSet* SceneRenderer::AmbientOcclusionPass::GetSceneBindingSet() const noexcept
	{
		if (m_FrameAvailable && m_FinalBindings)
			return m_FinalBindings.get();
		return m_FallbackBindings.get();
	}
}
