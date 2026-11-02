#include "GpuValidation/GpuSceneOutputValidation.h"

#include "Core/Application.h"
#include "Core/Log.h"
#include "GpuValidation/GpuSceneScenario.h"
#include "Renderer/SceneRenderer.h"

#include <algorithm>
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
			if (m_ToneMappingFrame >= 10)
				return UpdateRuntimeUIOverlayFrame();

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
		if (m_Mode != SceneOutputValidationMode::ToneMapping) return RenderHdrFrame();
		return m_ToneMappingFrame < 10 ? RenderToneMappingFrame() : RenderRuntimeUIOverlayFrame();
	}

	bool GpuSceneOutputValidation::IsComplete() const noexcept
	{
		return m_Mode == SceneOutputValidationMode::ToneMapping ? m_ToneMappingFrame >= 10 &&
			m_RuntimeUIOverlayFrame >= 13 && !m_RuntimeUIOverlayReadbackPending
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

	std::expected<bool, std::string> GpuSceneOutputValidation::UpdateRuntimeUIOverlayFrame()
	{
		if (std::chrono::steady_clock::now() - m_StartedAt > std::chrono::seconds(60))
			return std::unexpected("Timed out validating empty and visible runtime UI overlays");
		if (m_RuntimeUIOverlayReadbackPending)
		{
			if (auto Validated = ValidateRuntimeUIOverlayReadback(); !Validated)
				return std::unexpected(std::move(Validated.error()));
			++m_RuntimeUIOverlayFrame;
			m_RuntimeUIOverlayReadbackPending = false;
			if (m_RuntimeUIOverlayFrame >= 13) return true;
		}

		const uint32_t Width = m_RuntimeUIOverlayFrame >= 8 ? 128u : m_RuntimeUIOverlayFrame >= 6 ? 96u : 64u;
		constexpr uint32_t Height = 64;
		if (const auto Target = EnsureSdrValidationTarget(ColorTargetFormat::RGBA8_Srgb, Width, Height); !Target)
			return std::unexpected(Target.error());

		const bool CanvasPresent = m_RuntimeUIOverlayFrame != 0 && m_RuntimeUIOverlayFrame != 5 && m_RuntimeUIOverlayFrame != 12;
		std::vector<RuntimeUIDrawCommand> Commands;
		if (m_RuntimeUIOverlayFrame == 2 || (m_RuntimeUIOverlayFrame >= 3 && m_RuntimeUIOverlayFrame <= 4) ||
			(m_RuntimeUIOverlayFrame >= 7 && m_RuntimeUIOverlayFrame <= 9) || m_RuntimeUIOverlayFrame == 11)
		{
			RuntimeUIDrawCommand Quad;
			Quad.Element = UUID{ 0xb018000000004000ull, 0x9001 };
			Quad.Kind = RuntimeUIElementKind::Button;
			Quad.Bounds = { 0.0f, 0.0f, 64.0f, 64.0f };
			Quad.Clip = m_RuntimeUIOverlayFrame == 2 ? RuntimeUIRect{ 0.0f, 0.0f, 64.0f, 64.0f }
				: RuntimeUIRect{ 8.0f, 0.0f, 32.0f, 64.0f };
			Quad.Color = m_RuntimeUIOverlayFrame == 2 ? glm::vec4{ 1.0f, 1.0f, 1.0f, 0.0f }
				: glm::vec4{ 1.0f, 0.08f, 0.03f, 0.75f };
			Commands.push_back(Quad);
		}
		const RuntimeUIViewport Viewport{ { 64.0f, 64.0f }, Width, Height };
		m_Renderer.SetRuntimeUIFrame(Commands, Viewport, CanvasPresent);
		const auto Prepared = m_Renderer.PrepareScene(m_EmptyScene, static_cast<float>(Width) / static_cast<float>(Height));
		if (!Prepared)
			return std::unexpected("Could not prepare an empty scene for runtime UI overlay validation: " + Prepared.error().Message);
		if (m_Renderer.IsAssetPreparationPending()) return false;
		m_ScenePrepared = true;
		return false;
	}

	std::expected<void, std::string> GpuSceneOutputValidation::RenderRuntimeUIOverlayFrame()
	{
		if (m_RuntimeUIOverlayFrame >= 13) return {};
		if (!m_SdrTarget || !m_ScenePrepared)
			return std::unexpected("Runtime UI overlay validation has no prepared SDR target or scene");
		const RuntimeUIOverlayFrameStats PreviousStats = m_LastRuntimeUIOverlayStats;
		const auto Rendered = m_Renderer.RenderPreparedSceneToOutput(*m_SdrTarget);
		if (!Rendered)
			return std::unexpected("Could not render a runtime UI overlay validation frame: " + Rendered.error().Message);

		const uint32_t Width = m_SdrTarget->GetDescription().Width;
		const RuntimeUIOverlayFrameStats& Stats = Rendered->RuntimeUIOverlay;
		const bool VisibleCommands = m_RuntimeUIOverlayFrame == 2 ||
			(m_RuntimeUIOverlayFrame >= 3 && m_RuntimeUIOverlayFrame <= 4) ||
			(m_RuntimeUIOverlayFrame >= 7 && m_RuntimeUIOverlayFrame <= 9) || m_RuntimeUIOverlayFrame == 11;
		if (Rendered->RuntimeUIQuadCount != (VisibleCommands ? 1u : 0u) ||
			Rendered->RuntimeUIBatchCount != (VisibleCommands ? 1u : 0u) ||
			Stats.CanvasPresent != (m_RuntimeUIOverlayFrame != 0 && m_RuntimeUIOverlayFrame != 5) ||
			Stats.TransparentFallbackBound == VisibleCommands)
			return std::unexpected("Runtime UI overlay frame did not distinguish canvas presence, prepared quads, and fallback binding");
		if (!VisibleCommands && (Stats.BoundTextureWidth != 1 || Stats.BoundTextureHeight != 1 ||
			Stats.BoundTextureLogicalPayloadBytes != 4))
			return std::unexpected("An empty runtime UI frame did not bind the 1x1 transparent fallback texture");
		if (VisibleCommands && (Stats.TransparentFallbackBound || Stats.BoundTextureWidth != Width ||
			Stats.BoundTextureHeight != 64 || Stats.BoundTextureLogicalPayloadBytes != static_cast<uint64_t>(Width) * 64 * 8))
			return std::unexpected("Visible runtime UI did not bind a matching RGBA16F overlay target");

		const uint64_t ExpectedCreations = m_RuntimeUIOverlayFrame < 2 ? 0u
			: m_RuntimeUIOverlayFrame < 7 ? 1u : m_RuntimeUIOverlayFrame < 8 ? 2u : 3u;
		const uint64_t ExpectedClears = m_RuntimeUIOverlayFrame < 2 ? 0u
			: m_RuntimeUIOverlayFrame == 2 ? 1u : m_RuntimeUIOverlayFrame == 3 ? 2u
			: m_RuntimeUIOverlayFrame == 4 ? 3u : m_RuntimeUIOverlayFrame < 7 ? 3u
			: m_RuntimeUIOverlayFrame == 7 ? 4u : m_RuntimeUIOverlayFrame == 8 ? 5u
			: m_RuntimeUIOverlayFrame == 9 ? 6u : m_RuntimeUIOverlayFrame < 11 ? 6u
			: m_RuntimeUIOverlayFrame == 11 ? 7u : 7u;
		const uint32_t ExpectedRetainedWidth = m_RuntimeUIOverlayFrame < 2 ? 0u
			: m_RuntimeUIOverlayFrame < 7 ? 64u : m_RuntimeUIOverlayFrame < 8 ? 96u : 128u;
		if (Stats.OverlayTargetCreationCount != ExpectedCreations || Stats.OverlayClearCount != ExpectedClears ||
			Stats.RetainedOverlayWidth != ExpectedRetainedWidth ||
			Stats.RetainedOverlayHeight != (ExpectedRetainedWidth == 0 ? 0u : 64u) ||
			Stats.RetainedOverlayLogicalPayloadBytes != static_cast<uint64_t>(ExpectedRetainedWidth) * 64 * 8)
			return std::unexpected("Empty/visible UI transitions allocated or cleared an unexpected overlay target");
		if (m_RuntimeUIOverlayFrame == 4 &&
			(Stats.OverlayTargetCreationCount != PreviousStats.OverlayTargetCreationCount ||
			 Stats.ToneMappingBindingCreationCount != PreviousStats.ToneMappingBindingCreationCount ||
			 Stats.RuntimeUITextureBindingCreationCount != PreviousStats.RuntimeUITextureBindingCreationCount))
			return std::unexpected("An unchanged visible UI frame recreated an overlay target or binding");
		if (m_RuntimeUIOverlayFrame == 9 &&
			(Stats.OverlayTargetCreationCount != PreviousStats.OverlayTargetCreationCount ||
			 Stats.ToneMappingBindingCreationCount != PreviousStats.ToneMappingBindingCreationCount ||
			 Stats.RuntimeUITextureBindingCreationCount != PreviousStats.RuntimeUITextureBindingCreationCount))
			return std::unexpected("An unchanged 128x64 UI frame recreated an overlay target or binding");
		if ((m_RuntimeUIOverlayFrame == 5 || m_RuntimeUIOverlayFrame == 10 || m_RuntimeUIOverlayFrame == 12) &&
			(Stats.OverlayTargetCreationCount != PreviousStats.OverlayTargetCreationCount ||
			 Stats.OverlayClearCount != PreviousStats.OverlayClearCount))
			return std::unexpected("An empty UI frame cleared or recreated the retained full-size overlay");

		m_LastRuntimeUIOverlayStats = Stats;
		m_RuntimeUIOverlayReadbackPending = true;
		m_ScenePrepared = false;
		return {};
	}

	std::expected<void, std::string> GpuSceneOutputValidation::ValidateRuntimeUIOverlayReadback()
	{
		if (!m_SdrTarget || !m_SdrTarget->GetColorTexture())
			return std::unexpected("Runtime UI overlay readback target was retired before readback");
		const auto Readback = m_Runtime.ReadTextureForValidation(*m_SdrTarget->GetColorTexture());
		const uint32_t ExpectedWidth = m_RuntimeUIOverlayFrame >= 8 ? 128u : m_RuntimeUIOverlayFrame >= 6 ? 96u : 64u;
		if (!Readback || Readback->Format != TextureFormat::RGBA8_Srgb || Readback->Width != ExpectedWidth ||
			Readback->Height != 64 || Readback->Pixels.size() != static_cast<size_t>(ExpectedWidth) * 64 * 4)
			return std::unexpected(Readback ? "Runtime UI overlay readback has unexpected format or extent"
				: "Could not read tone-mapped runtime UI output: " + Readback.error().Message);

		const auto Pixel = [&](const std::vector<std::byte>& Pixels, uint32_t Width, uint32_t X, uint32_t Y)
		{
			const size_t Offset = (static_cast<size_t>(Y) * Width + X) * 4;
			return std::array<uint8_t, 4>{ std::to_integer<uint8_t>(Pixels[Offset]),
				std::to_integer<uint8_t>(Pixels[Offset + 1]), std::to_integer<uint8_t>(Pixels[Offset + 2]),
				std::to_integer<uint8_t>(Pixels[Offset + 3]) };
		};
		const auto SamePixels = [](const std::vector<std::byte>& A, const std::vector<std::byte>& B)
		{
			return A.size() == B.size() && std::equal(A.begin(), A.end(), B.begin());
		};
		const auto SameUniformBackground = [&](const std::vector<std::byte>& Pixels, uint32_t Width)
		{
			if (m_RuntimeUIBaseline96.empty() || Width == 0) return false;
			const auto Background = Pixel(m_RuntimeUIBaseline96, 96, 0, 0);
			for (size_t Offset = 0; Offset + 4 <= Pixels.size(); Offset += 4)
				if (std::to_integer<uint8_t>(Pixels[Offset]) != Background[0] ||
					std::to_integer<uint8_t>(Pixels[Offset + 1]) != Background[1] ||
					std::to_integer<uint8_t>(Pixels[Offset + 2]) != Background[2] ||
					std::to_integer<uint8_t>(Pixels[Offset + 3]) != Background[3])
					return false;
			return Pixels.size() == static_cast<size_t>(Width) * 64 * 4;
		};
		const auto VerifyVisibleClip = [&](const std::vector<std::byte>& Baseline, uint32_t BaselineWidth,
			uint32_t InsideX, uint32_t OutsideX)
		{
			const auto BaseInside = Pixel(Baseline, BaselineWidth, InsideX, 32);
			const auto DrawnInside = Pixel(Readback->Pixels, ExpectedWidth, InsideX, 32);
			const auto BaseOutside = Pixel(Baseline, BaselineWidth, OutsideX, 32);
			const auto DrawnOutside = Pixel(Readback->Pixels, ExpectedWidth, OutsideX, 32);
			return DrawnInside[0] > BaseInside[0] + 20 && DrawnInside[0] > DrawnInside[1] + 35 &&
				DrawnOutside == BaseOutside;
		};

		switch (m_RuntimeUIOverlayFrame)
		{
		case 0:
			m_RuntimeUIBaseline64 = Readback->Pixels;
			if (m_LastRuntimeUIOverlayStats.CanvasPresent || !m_LastRuntimeUIOverlayStats.TransparentFallbackBound ||
				m_LastRuntimeUIOverlayStats.RetainedOverlayWidth != 0)
				return std::unexpected("A scene without a UI canvas allocated or bound a full-size UI overlay");
			break;
		case 1:
		case 2:
		case 5:
			if (!SamePixels(Readback->Pixels, m_RuntimeUIBaseline64))
				return std::unexpected("An empty or transparent UI frame changed the tone-mapped scene output");
			break;
		case 3:
			if (!VerifyVisibleClip(m_RuntimeUIBaseline64, 64, 20, 48))
				return std::unexpected("A visible UI quad failed the 64x64 rendered-pixel or clipping check");
			m_RuntimeUIVisibleFrame = Readback->Pixels;
			break;
		case 4:
			if (!SamePixels(Readback->Pixels, m_RuntimeUIVisibleFrame))
				return std::unexpected("An unchanged visible UI frame changed its rendered pixels");
			break;
		case 6:
			m_RuntimeUIBaseline96 = Readback->Pixels;
			break;
		case 7:
			if (!VerifyVisibleClip(m_RuntimeUIBaseline96, 96, 42, 60))
				return std::unexpected("A 1.5x output scale did not scale the UI scissor with the rendered quad");
			break;
		case 8:
			if (!VerifyVisibleClip(m_RuntimeUIBaseline96, 96, 42, 80))
				return std::unexpected("A 96x64 to 128x64 resize while visible did not preserve clipping and output composition");
			m_RuntimeUIVisibleFrame = Readback->Pixels;
			break;
		case 9:
			if (!SamePixels(Readback->Pixels, m_RuntimeUIVisibleFrame))
				return std::unexpected("An unchanged 128x64 UI frame changed its rendered pixels");
			break;
		case 10:
		case 12:
			if (!SameUniformBackground(Readback->Pixels, 128))
				return std::unexpected("Empty UI after a visible frame left ghost pixels in the resized output");
			break;
		case 11:
			if (!VerifyVisibleClip(m_RuntimeUIBaseline96, 96, 42, 80))
				return std::unexpected("UI did not reappear correctly after an empty interval");
			m_RuntimeUIVisibleFrame = Readback->Pixels;
			break;
		default:
			return std::unexpected("Runtime UI overlay validation advanced beyond its declared phase set");
		}

		if (m_RuntimeUIOverlayFrame == 12)
			PF_INFO("RUNTIME_UI_OVERLAY_PATH=PASS; no-canvas and empty frames used a 1x1/4-byte fallback; "
				"retained RGBA16F overlays were 64x64/32768, 96x64/49152, and 128x64/65536 logical bytes; "
				"3 target creations and 7 clears; empty, transparent, visible, clipped, resized, disappearance, "
				"and reappearance pixels verified");
		return {};
	}
} // namespace PulseForge::GpuValidation
