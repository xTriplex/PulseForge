#include "GpuValidation/GpuSceneOutputValidation.h"

#include "Core/Application.h"
#include "Core/Log.h"
#include "GpuValidation/GpuSceneScenario.h"
#include "Renderer/SceneRenderer.h"

#include <array>
#include <cstddef>
#include <utility>

namespace PulseForge::GpuValidation
{
	GpuSceneOutputValidation::GpuSceneOutputValidation(Application& Runtime, Scene& StartupScene, SceneRenderer& Renderer,
		const BindingLayoutHandle& SamplingLayout, const SamplerHandle& SamplingSampler, const GraphicsPipelineHandle& SamplingPipeline,
		const BufferHandle& FullscreenTriangle, SceneOutputValidationMode Mode, std::chrono::steady_clock::time_point StartedAt) noexcept
		: m_Runtime(Runtime), m_StartupScene(StartupScene), m_Renderer(Renderer), m_SamplingLayout(SamplingLayout),
		  m_SamplingSampler(SamplingSampler), m_SamplingPipeline(SamplingPipeline), m_FullscreenTriangle(FullscreenTriangle), m_Mode(Mode),
		  m_StartedAt(StartedAt)
	{
	}

	void GpuSceneOutputValidation::Initialize()
	{
		BuildGpuValidationSceneScenario(m_StartupScene, m_EmptyScene, false, false, false);
		BuildGpuValidationSceneScenario(m_StartupScene, m_EnvironmentOnlyScene, false, true, false);
		BuildGpuValidationSceneScenario(m_StartupScene, m_GeometryOnlyScene, true, false, false);
		BuildGpuValidationSceneScenario(m_StartupScene, m_DirectionalOnlyScene, true, false, true);
	}

	std::expected<bool, std::string> GpuSceneOutputValidation::Update()
	{
		if (IsComplete()) return true;

		m_ScenePrepared = false;
		if (m_Mode == SceneOutputValidationMode::ToneMapping)
		{
			if (std::chrono::steady_clock::now() - m_StartedAt > std::chrono::seconds(60))
				return std::unexpected("Timed out preparing or rendering tone-mapped scene outputs");

			const bool Offscreen = m_ToneMappingFrame >= 2;
			uint32_t Width = 0;
			uint32_t Height = 0;
			if (Offscreen)
			{
				Width = m_ToneMappingFrame >= 4 ? 96u : 64u;
				Height = 64;
			}
			else
			{
				const auto Extent = m_Runtime.GetWindow().GetFramebufferSize();
				Width = Extent.first;
				Height = Extent.second;
			}
			if (Width == 0 || Height == 0) return false;

			Scene* SceneToPrepare = &m_StartupScene;
			if (m_ToneMappingFrame == 6) SceneToPrepare = &m_EnvironmentOnlyScene;
			if (m_ToneMappingFrame == 7) SceneToPrepare = &m_GeometryOnlyScene;
			if (m_ToneMappingFrame == 8) SceneToPrepare = &m_EmptyScene;
			if (m_ToneMappingFrame == 9) SceneToPrepare = &m_DirectionalOnlyScene;
			const auto Prepared = m_Renderer.PrepareScene(*SceneToPrepare, static_cast<float>(Width) / static_cast<float>(Height));
			if (!Prepared) return std::unexpected("Could not prepare the scene for tone mapping: " + Prepared.error().Message);

			const bool HasEnvironment = m_ToneMappingFrame < 7;
			if (m_Renderer.IsAssetPreparationPending()) return false;
			if (HasEnvironment && !m_Renderer.HasPreparedEnvironmentLighting())
				return std::unexpected("Tone-mapping scenario did not prepare its environment lighting");
			m_ScenePrepared = true;
			return false;
		}

		if (std::chrono::steady_clock::now() - m_StartedAt > std::chrono::seconds(40))
			return std::unexpected("Timed out preparing or rendering the sample HDR scene");

		const auto [Width, Height] = m_Runtime.GetWindow().GetFramebufferSize();
		if (Width == 0 || Height == 0) return false;
		Scene* SceneToPrepare = m_SceneCompletedFrames < 4 ? &m_StartupScene : &GetScenarioScene(m_ScenarioIndex);
		AmbientOcclusionSettings AoSettings = m_Renderer.GetAmbientOcclusionSettings();
		AoSettings.Enabled = !(m_SceneCompletedFrames >= 4 && m_ScenarioIndex == 2);
		if (!m_Renderer.SetAmbientOcclusionSettings(AoSettings))
			return std::unexpected("Could not select the validation scene's ambient-occlusion mode");
		const auto Prepared = m_Renderer.PrepareScene(*SceneToPrepare, static_cast<float>(Width) / static_cast<float>(Height));
		if (!Prepared) return std::unexpected("Could not prepare the bundled scene for HDR rendering: " + Prepared.error().Message);

		const bool SceneHasEnvironment = m_SceneCompletedFrames < 4 || m_ScenarioIndex == 1;
		if (m_Renderer.IsAssetPreparationPending()) return false;
		if (SceneHasEnvironment && !m_Renderer.HasPreparedEnvironmentLighting())
			return std::unexpected("Bundled validation scene did not produce prepared environment lighting");
		m_ScenePrepared = true;
		return false;
	}

	std::expected<void, std::string> GpuSceneOutputValidation::Render()
	{
		if (!m_ScenePrepared || IsComplete()) return {};
		return m_Mode == SceneOutputValidationMode::ToneMapping ? RenderToneMappingFrame() : RenderHdrFrame();
	}

	bool GpuSceneOutputValidation::IsComplete() const noexcept
	{
		return m_Mode == SceneOutputValidationMode::ToneMapping ? m_ToneMappingFrame >= 10
																: m_SceneCompletedFrames >= 4 && m_ScenarioIndex >= 4;
	}

	Scene& GpuSceneOutputValidation::GetScenarioScene(size_t Index)
	{
		switch (Index)
		{
		case 0:
			return m_EmptyScene;
		case 1:
			return m_EnvironmentOnlyScene;
		case 2:
			return m_GeometryOnlyScene;
		default:
			return m_DirectionalOnlyScene;
		}
	}

	std::expected<void, std::string> GpuSceneOutputValidation::RenderHdrFrame()
	{
		if (m_SceneCompletedFrames >= 4 && m_ScenarioIndex >= 4) return {};
		if (m_SceneCompletedFrames == 0)
		{
			const auto InvalidExtent = m_Renderer.RenderPreparedSceneToHdr(0, 64);
			if (InvalidExtent || InvalidExtent.error().Code != SceneRendererErrorCode::ResourceCreationFailed)
				return std::unexpected("HDR target did not reject a zero-width extent");
		}
		if (m_SceneCompletedFrames == 2) m_SamplingBindingSet.reset();

		const bool SampleValidationScene = m_SceneCompletedFrames < 4;
		const uint32_t Width = SampleValidationScene && m_SceneCompletedFrames < 2 ? 64u : 96u;
		const uint32_t Height = 64u;
		auto Rendered = m_Renderer.RenderPreparedSceneToHdr(Width, Height);
		if (!Rendered) return std::unexpected("SceneRenderer HDR rendering failed: " + Rendered.error().Message);
		const Texture* Color = Rendered->ColorTexture;
		const bool ExpectedGeometry = SampleValidationScene || m_ScenarioIndex >= 2;
		const bool ExpectedEnvironment = SampleValidationScene || m_ScenarioIndex == 1;
		if (!Color || !Rendered->DepthTexture || Rendered->Width != Width || Rendered->Height != Height ||
			(Rendered->GeometryDrawCount > 0) != ExpectedGeometry || Rendered->EnvironmentBackgroundDrawn != ExpectedEnvironment)
			return std::unexpected("SceneRenderer HDR result did not match expected geometry/environment participation");

		const TextureDesc& ColorDescription = Color->GetDescription();
		if (ColorDescription.Width != Width || ColorDescription.Height != Height ||
			ColorDescription.Format != TextureFormat::RGBA16_Float ||
			!HasTextureUsage(ColorDescription.Usage, TextureUsage::ShaderResource) ||
			!HasTextureUsage(ColorDescription.Usage, TextureUsage::ColorAttachment))
			return std::unexpected("SceneRenderer HDR color texture has incompatible metadata or usage");
		const TextureDesc& DepthDescription = Rendered->DepthTexture->GetDescription();
		if (DepthDescription.Width != Width || DepthDescription.Height != Height ||
			DepthDescription.Format != TextureFormat::Depth32Float ||
			!HasTextureUsage(DepthDescription.Usage, TextureUsage::DepthStencilAttachment))
			return std::unexpected("SceneRenderer HDR target has incompatible D32 depth attachment metadata");

		if (SampleValidationScene && m_SceneCompletedFrames == 0) m_FirstHdrTexture = Color;
		else if (SampleValidationScene && m_SceneCompletedFrames == 1 && Color != m_FirstHdrTexture)
			return std::unexpected("SceneRenderer recreated its HDR target at an unchanged extent");
		else if (SampleValidationScene && m_SceneCompletedFrames == 2 && Color == m_FirstHdrTexture)
			return std::unexpected("SceneRenderer did not replace its HDR target after resize");
		else if (SampleValidationScene && m_SceneCompletedFrames == 3 && Color != m_ResizedHdrTexture)
			return std::unexpected("SceneRenderer recreated its resized HDR target at an unchanged extent");
		if (SampleValidationScene && m_SceneCompletedFrames == 2) m_ResizedHdrTexture = Color;

		if (m_BoundHdrTexture != Color)
		{
			m_SamplingBindingSet.reset();
			BindingSetDesc SamplingDescription;
			SamplingDescription.Layout = m_SamplingLayout;
			SamplingDescription.Textures.push_back({ 0, *Color, BindingResourceType::Texture2D });
			SamplingDescription.Samplers.push_back({ 0, *m_SamplingSampler });
			auto SamplingSet = m_Runtime.CreateBindingSet(SamplingDescription);
			if (!SamplingSet)
				return std::unexpected("Could not bind SceneRenderer HDR output for sampling: " + SamplingSet.error().Message);
			m_SamplingBindingSet = std::move(*SamplingSet);
			m_BoundHdrTexture = Color;
		}
		const std::array<const BindingSet*, 1> BindingSets = { m_SamplingBindingSet.get() };
		const GraphicsResult SamplingDraw = m_Runtime.Draw(*m_SamplingPipeline, *m_FullscreenTriangle, { 3, 1, 0, 0 }, BindingSets);
		if (!SamplingDraw) return std::unexpected("Could not sample SceneRenderer HDR output: " + SamplingDraw.error().Message);

		if (SampleValidationScene) ++m_SceneCompletedFrames;
		else ++m_ScenarioIndex;
		PF_INFO("Scene HDR validation: scenario {0}, extent {1}x{2}, {3} indexed geometry draw(s), environment background {4}",
			SampleValidationScene ? "bundled" : m_ScenarioNames[m_ScenarioIndex - 1], Width, Height, Rendered->GeometryDrawCount,
			Rendered->EnvironmentBackgroundDrawn ? "drawn" : "not present");
		return {};
	}

	std::expected<void, std::string> GpuSceneOutputValidation::EnsureSdrValidationTarget(
		ColorTargetFormat Format, uint32_t Width, uint32_t Height)
	{
		if (m_SdrTarget && m_SdrTarget->GetDescription().Width == Width && m_SdrTarget->GetDescription().Height == Height &&
			m_SdrTarget->GetDescription().ColorFormat == Format)
			return {};
		m_SamplingBindingSet.reset();
		m_BoundOutputTexture = nullptr;
		RenderTargetDesc Description;
		Description.Width = Width;
		Description.Height = Height;
		Description.ColorFormat = Format;
		Description.DepthMode = Format == ColorTargetFormat::RGBA8_UNorm ? DepthAttachmentMode::None : DepthAttachmentMode::Attachment;
		Description.DebugName =
			Format == ColorTargetFormat::RGBA8_Srgb ? "Tone-mapping validation sRGB output" : "Tone-mapping validation UNORM output";
		auto Target = m_Runtime.CreateRenderTarget(Description);
		if (!Target) return std::unexpected("Could not create SDR validation target: " + Target.error().Message);
		m_SdrTarget = std::move(*Target);
		return {};
	}

	std::expected<void, std::string> GpuSceneOutputValidation::RenderToneMappingFrame()
	{
		if (m_ToneMappingFrame >= 10) return {};
		const bool DirectToSwapchain = m_ToneMappingFrame < 2;
		const bool UseUnormTarget = m_ToneMappingFrame == 5;
		const uint32_t Width = DirectToSwapchain ? m_Runtime.GetWindow().GetFramebufferSize().first : (m_ToneMappingFrame >= 4 ? 96u : 64u);
		const uint32_t Height = DirectToSwapchain ? m_Runtime.GetWindow().GetFramebufferSize().second : 64u;
		if (Width == 0 || Height == 0) return {};

		const std::array<float, 10> Exposures = { 0.0f, 1.0f, -2.0f, 2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
		if (!m_Renderer.SetToneMappingSettings({ Exposures[m_ToneMappingFrame] }))
			return std::unexpected("SceneRenderer rejected a valid GPU-validation exposure setting");

		SceneRenderOutputResult Output;
		const Texture* OutputTexture = nullptr;
		if (DirectToSwapchain)
		{
			auto Rendered = m_Renderer.RenderPreparedSceneToOutput();
			if (!Rendered) return std::unexpected("SceneRenderer swapchain tonemapping failed: " + Rendered.error().Message);
			Output = *Rendered;
			if (Output.DestinationFormat != ColorTargetFormat::Swapchain ||
				Output.ShaderSrgbEncoded != (m_Runtime.GetOutputColorEncoding() == OutputColorEncoding::UnormAttachment))
				return std::unexpected("Swapchain tone-mapping output encoding did not match the active attachment");
		}
		else
		{
			const ColorTargetFormat Format = UseUnormTarget ? ColorTargetFormat::RGBA8_UNorm : ColorTargetFormat::RGBA8_Srgb;
			if (const auto Target = EnsureSdrValidationTarget(Format, Width, Height); !Target) return std::unexpected(Target.error());
			auto Rendered = m_Renderer.RenderPreparedSceneToOutput(*m_SdrTarget);
			if (!Rendered) return std::unexpected("SceneRenderer offscreen tonemapping failed: " + Rendered.error().Message);
			Output = *Rendered;
			OutputTexture = m_SdrTarget->GetColorTexture();
			const TextureFormat ExpectedFormat = UseUnormTarget ? TextureFormat::RGBA8_UNorm : TextureFormat::RGBA8_Srgb;
			if (!OutputTexture || Output.DestinationFormat != Format || Output.ShaderSrgbEncoded != UseUnormTarget ||
				m_SdrTarget->GetDescription().DepthMode != (UseUnormTarget ? DepthAttachmentMode::None : DepthAttachmentMode::Attachment) ||
				OutputTexture->GetDescription().Format != ExpectedFormat || OutputTexture->GetDescription().Width != Width ||
				OutputTexture->GetDescription().Height != Height)
				return std::unexpected("Tone-mapped SDR target format, extent, or transfer-function decision was incorrect");
			if (Output.GeometryDrawCount != ((m_ToneMappingFrame == 6 || m_ToneMappingFrame == 8) ? 0u : 3u))
				return std::unexpected("Tone-mapping scene variant submitted an unexpected indexed geometry count");
			if (m_BoundOutputTexture != OutputTexture)
			{
				m_SamplingBindingSet.reset();
				BindingSetDesc Description;
				Description.Layout = m_SamplingLayout;
				Description.Textures.push_back({ 0, *OutputTexture, BindingResourceType::Texture2D });
				Description.Samplers.push_back({ 0, *m_SamplingSampler });
				auto Set = m_Runtime.CreateBindingSet(Description);
				if (!Set) return std::unexpected("Could not bind tone-mapped SDR output for validation sampling: " + Set.error().Message);
				m_SamplingBindingSet = std::move(*Set);
				m_BoundOutputTexture = OutputTexture;
			}
			const std::array<const BindingSet*, 1> Sets = { m_SamplingBindingSet.get() };
			if (const auto Draw = m_Runtime.Draw(*m_SamplingPipeline, *m_FullscreenTriangle, { 3, 1, 0, 0 }, Sets); !Draw)
				return std::unexpected("Could not sample tone-mapped SDR output: " + Draw.error().Message);
		}

		if (m_ToneMappingFrame == 0) m_SwapchainHdrGeneration = Output.HdrTargetGeneration;
		else if (m_ToneMappingFrame == 1 && Output.HdrTargetGeneration != m_SwapchainHdrGeneration)
			return std::unexpected("Unchanged swapchain extent recreated the HDR scene target");
		if (m_ToneMappingFrame == 2) m_SrgbHdrGeneration = Output.HdrTargetGeneration;
		else if (m_ToneMappingFrame == 3 && Output.HdrTargetGeneration != m_SrgbHdrGeneration)
			return std::unexpected("Exposure changes recreated the unchanged HDR target");
		if (m_ToneMappingFrame == 4 && Output.HdrTargetGeneration == m_SrgbHdrGeneration)
			return std::unexpected("HDR target generation did not change after the 64x64 to 96x64 resize");
		if (m_ToneMappingFrame == 4) m_ResizedHdrGeneration = Output.HdrTargetGeneration;
		else if (m_ToneMappingFrame >= 5 && Output.HdrTargetGeneration != m_ResizedHdrGeneration)
			return std::unexpected("Changing output format or scene scenario recreated the same-size HDR target");

		PF_INFO("Tonemapping validation frame {0}: {1}, EV {2}, {3}x{4}, HDR generation {5}, shader sRGB encode {6}", m_ToneMappingFrame,
			DirectToSwapchain ? "swapchain"
			: UseUnormTarget  ? "RGBA8_UNorm"
							  : "RGBA8_Srgb",
			Exposures[m_ToneMappingFrame], Width, Height, Output.HdrTargetGeneration, Output.ShaderSrgbEncoded);
		++m_ToneMappingFrame;
		return {};
	}
} // namespace PulseForge::GpuValidation
