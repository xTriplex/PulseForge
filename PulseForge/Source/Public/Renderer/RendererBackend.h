#pragma once

#include "Renderer/Buffer.h"
#include "Renderer/Binding.h"
#include "Renderer/Graphics.h"
#include "Renderer/RenderTarget.h"
#include "Renderer/DepthCubemap.h"
#include "Renderer/RenderDevice.h"

#include <memory>
#include <expected>
#include <string>
#include <string_view>
#include <cstdint>
#include <span>
#include "Renderer/RendererAPI.h"

namespace PulseForge
{
	class Window;

	class RendererBackend
	{
	public:
		virtual ~RendererBackend() = default;

		[[nodiscard]] virtual OutputColorEncoding GetOutputColorEncoding() const noexcept = 0;
		[[nodiscard]] virtual std::string_view GetRendererHardwareName() const noexcept { return "not reported"; }
		[[nodiscard]] virtual bool SetGpuFrameTimingEnabled(bool Enabled, uint64_t SessionId = 0) noexcept
		{
			(void)Enabled;
			(void)SessionId;
			return false;
		}
		[[nodiscard]] virtual std::string_view GetGpuFrameTimingUnavailableReason() const noexcept
		{
			return "GPU frame timing is unavailable on this renderer.";
		}
		[[nodiscard]] virtual bool IsGpuFrameTimingEnabled() const noexcept { return false; }
		[[nodiscard]] virtual bool IsGpuFrameTimingSupported() const noexcept { return false; }
		[[nodiscard]] virtual bool HasGpuFrameTimingFailed() const noexcept { return false; }
		[[nodiscard]] virtual size_t ReadCompletedGpuFrameTimings(std::span<GpuFrameTiming> Destination) noexcept
		{
			(void)Destination;
			return 0;
		}
		[[nodiscard]] virtual uint64_t GetCompletedGpuFrameTimingEvictedCount() const noexcept { return 0; }
		[[nodiscard]] virtual uint64_t GetGpuFrameTimingInvalidResultCount() const noexcept { return 0; }
		virtual bool BeginFrame() = 0;
		virtual void EndFrame() = 0;
		[[nodiscard]] virtual std::expected<uint64_t, std::string> CreatePlatformPresentationTarget(void* NativeWindow)
		{
			(void)NativeWindow;
			return std::unexpected("This renderer does not support native platform-window presentation");
		}
		[[nodiscard]] virtual bool BeginPlatformPresentationTarget(uint64_t TargetID)
		{
			(void)TargetID;
			return false;
		}
		virtual void EndPlatformPresentationTarget(uint64_t TargetID) { (void)TargetID; }
		[[nodiscard]] virtual bool DestroyPlatformPresentationTarget(uint64_t TargetID)
		{
			(void)TargetID;
			return true;
		}
		[[nodiscard]] virtual bool HasPlatformPresentationTargets() const noexcept { return false; }
		virtual void SetExternalPlatformPresentationTargetCount(uint32_t Count) noexcept { (void)Count; }
#if defined(PF_DEBUG)
		[[nodiscard]] virtual bool InjectNextPlatformAcquireOutOfDateForTesting(uint64_t TargetID) noexcept
		{
			(void)TargetID;
			return false;
		}
		[[nodiscard]] virtual bool InjectNextPlatformImageAllocationFailureForTesting() noexcept { return false; }
#endif
		virtual RenderTargetCreateResult CreateRenderTarget(const RenderTargetDesc& Description) = 0;
		virtual DepthCubemapCreateResult CreateDepthCubemap(uint32_t Resolution, const std::string& DebugName) = 0;
		virtual GraphicsResult BeginRenderTarget(
			const RenderTarget& Target,
			const RenderTargetClearValue& ClearValue) = 0;
		virtual GraphicsResult EndRenderTarget() = 0;
		virtual BufferCreateResult CreateBuffer(
			const BufferDesc& Description,
			std::span<const std::byte> InitialData) = 0;
		virtual BufferUpdateResult WriteBuffer(
			const Buffer& Target,
			uint64_t DestinationOffset,
			std::span<const std::byte> Data) = 0;
		virtual TextureCreateResult CreateTexture(
			const TextureDesc& Description,
			std::span<const TextureSubresourceData> InitialData) = 0;
		virtual TextureReadbackResult ReadTextureForValidation(const Texture& Source, uint32_t ArraySlice)
		{
			(void)Source;
			(void)ArraySlice;
			return std::unexpected(TextureError{
				TextureErrorCode::UnsupportedFeature,
				"This renderer does not support validation-only texture readback"
			});
		}
		virtual SamplerCreateResult CreateSampler(const SamplerDesc& Description) = 0;
		virtual BindingLayoutCreateResult CreateBindingLayout(const BindingLayoutDesc& Description) = 0;
		virtual BindingSetCreateResult CreateBindingSet(const BindingSetDesc& Description) = 0;
		virtual ShaderCreateResult CreateShader(
			const ShaderDesc& Description,
			std::span<const std::byte> Bytecode) = 0;
		virtual GraphicsPipelineCreateResult CreateGraphicsPipeline(
			const GraphicsPipelineDesc& Description) = 0;
		virtual GraphicsResult Draw(
			const GraphicsPipeline& Pipeline,
			const Buffer& VertexBuffer,
			const DrawArguments& Arguments,
			std::span<const BindingSet* const> BindingSets) = 0;
		virtual GraphicsResult DrawIndexed(
			const GraphicsPipeline& Pipeline,
			const Buffer& VertexBuffer,
			const Buffer& IndexBuffer,
			const DrawIndexedArguments& Arguments,
			std::span<const BindingSet* const> BindingSets) = 0;

		static std::unique_ptr<RendererBackend> Create(RendererAPI API, Window& Window);
	};
}
