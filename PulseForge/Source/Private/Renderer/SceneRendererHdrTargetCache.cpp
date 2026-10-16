#include "Renderer/SceneRendererHdrTargetCache.h"

#include "Core/Log.h"

#include <limits>
#include <utility>

namespace PulseForge
{
	namespace
	{
		SceneRendererError MakeHdrTargetResourceError(std::string Message)
		{
			return { SceneRendererErrorCode::ResourceCreationFailed, {}, {}, std::move(Message) };
		}
	} // namespace

	SceneRenderer::HdrTargetCache::HdrTargetCache(RenderDevice& Runtime) noexcept : m_Runtime(Runtime)
	{
	}

	bool SceneRenderer::HdrTargetCache::NeedsReplacement(uint32_t Width, uint32_t Height) const noexcept
	{
		return Width != 0 && Height != 0 && !CanReuse(Width, Height);
	}

	bool SceneRenderer::HdrTargetCache::CanReuse(uint32_t Width, uint32_t Height) const noexcept
	{
		if (!m_Target || !MatchesRenderTargetConfiguration(
							 m_Target->GetDescription(), Width, Height, ColorTargetFormat::RGBA16_Float, DepthAttachmentMode::Attachment))
			return false;

		const Texture* Color = m_Target->GetColorTexture();
		const Texture* Depth = m_Target->GetDepthTexture();
		return Color && Depth && Color->GetDescription().Format == TextureFormat::RGBA16_Float &&
			   Depth->GetDescription().Format == TextureFormat::Depth32Float;
	}

	std::expected<SceneRenderer::HdrTargetCache::EnsureResult, SceneRendererError> SceneRenderer::HdrTargetCache::Ensure(
		uint32_t Width, uint32_t Height)
	{
		if (Width == 0 || Height == 0) return std::unexpected(MakeHdrTargetResourceError("HDR scene target dimensions must be non-zero"));

		if (CanReuse(Width, Height)) return EnsureResult{ .Target = m_Target.get(), .Generation = m_Generation };

		RenderTargetDesc Description;
		Description.Width = Width;
		Description.Height = Height;
		Description.ColorFormat = ColorTargetFormat::RGBA16_Float;
		Description.DepthMode = DepthAttachmentMode::Attachment;
		Description.DebugName = "PulseForge persistent HDR scene target";
		auto Replacement = m_Runtime.CreateRenderTarget(Description);
		if (!Replacement)
			return std::unexpected(MakeHdrTargetResourceError("Could not create HDR scene target: " + Replacement.error().Message));

		const Texture* Color = (*Replacement)->GetColorTexture();
		const Texture* Depth = (*Replacement)->GetDepthTexture();
		if (!Color || !Depth || Color->GetDescription().Width != Width || Color->GetDescription().Height != Height ||
			Color->GetDescription().Format != TextureFormat::RGBA16_Float ||
			!HasTextureUsage(Color->GetDescription().Usage, TextureUsage::ColorAttachment) ||
			!HasTextureUsage(Color->GetDescription().Usage, TextureUsage::ShaderResource) || Depth->GetDescription().Width != Width ||
			Depth->GetDescription().Height != Height || Depth->GetDescription().Format != TextureFormat::Depth32Float ||
			!HasTextureUsage(Depth->GetDescription().Usage, TextureUsage::DepthStencilAttachment))
		{
			return std::unexpected(MakeHdrTargetResourceError("HDR scene target returned incompatible color or depth attachments"));
		}

		m_Target = std::move(*Replacement);
		m_Generation = m_Generation == (std::numeric_limits<uint64_t>::max)() ? 1 : m_Generation + 1;
		PF_CORE_INFO("Created/resized persistent RGBA16F HDR scene target to {0}x{1}", Width, Height);
		return EnsureResult{ .Target = m_Target.get(), .Generation = m_Generation };
	}
} // namespace PulseForge
