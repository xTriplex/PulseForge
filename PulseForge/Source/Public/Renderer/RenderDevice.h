#pragma once

#include "Renderer/Binding.h"
#include "Core/Diagnostics.h"
#include "Renderer/DepthCubemap.h"
#include "Renderer/Graphics.h"
#include "Renderer/Mesh.h"
#include "Renderer/RenderTarget.h"
#include "Renderer/RendererAPI.h"

#include <cstdint>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace PulseForge
{
	struct GpuFrameTiming
	{
		uint64_t SessionId = 0;
		uint64_t FrameSequence = 0;
		uint64_t DurationNanoseconds = 0;
	};

	// Rendering systems use this validated resource and draw capability without depending on Application's
	// window, input, event loop, or layer ownership. The caller controls the active frame and device lifetime.
	class PULSEFORGE_API RenderDevice
	{
	public:
		virtual ~RenderDevice() = default;
		// Optional application-lifetime sink; renderer clients must not retain it in asynchronous jobs.
		[[nodiscard]] virtual DiagnosticRecorder* GetDiagnosticRecorder() noexcept { return nullptr; }

		[[nodiscard]] virtual std::pair<unsigned int, unsigned int> GetOutputFramebufferSize() const noexcept = 0;
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
		[[nodiscard]] virtual size_t ReadCompletedGpuFrameTimings(std::span<GpuFrameTiming> Destination) noexcept
		{
			(void)Destination;
			return 0;
		}
		[[nodiscard]] virtual uint64_t GetCompletedGpuFrameTimingEvictedCount() const noexcept { return 0; }
		[[nodiscard]] virtual RenderTargetCreateResult CreateRenderTarget(const RenderTargetDesc& Description) = 0;
		[[nodiscard]] virtual DepthCubemapCreateResult CreateDepthCubemap(uint32_t Resolution, const std::string& DebugName) = 0;
		[[nodiscard]] virtual GraphicsResult BeginRenderTarget(
			const RenderTarget& Target, const RenderTargetClearValue& ClearValue = {}) = 0;
		[[nodiscard]] virtual GraphicsResult EndRenderTarget() = 0;
		[[nodiscard]] virtual BufferCreateResult CreateBuffer(
			const BufferDesc& Description, std::span<const std::byte> InitialData = {}) = 0;
		[[nodiscard]] virtual BufferUpdateResult WriteBuffer(
			const Buffer& Target, uint64_t DestinationOffset, std::span<const std::byte> Data) = 0;
		[[nodiscard]] virtual TextureCreateResult CreateTexture(
			const TextureDesc& Description, std::span<const TextureSubresourceData> InitialData) = 0;
		[[nodiscard]] TextureCreateResult CreateTexture(const TextureDesc& Description, std::span<const std::byte> InitialData);
		[[nodiscard]] virtual SamplerCreateResult CreateSampler(const SamplerDesc& Description) = 0;
		[[nodiscard]] virtual BindingLayoutCreateResult CreateBindingLayout(const BindingLayoutDesc& Description) = 0;
		[[nodiscard]] virtual BindingSetCreateResult CreateBindingSet(const BindingSetDesc& Description) = 0;
		[[nodiscard]] virtual ShaderCreateResult CreateShader(const ShaderDesc& Description, std::span<const std::byte> Bytecode) = 0;
		[[nodiscard]] virtual GraphicsPipelineCreateResult CreateGraphicsPipeline(const GraphicsPipelineDesc& Description) = 0;
		[[nodiscard]] virtual GraphicsResult Draw(const GraphicsPipeline& Pipeline, const Buffer& VertexBuffer,
			const DrawArguments& Arguments, std::span<const BindingSet* const> BindingSets = {}) = 0;
		[[nodiscard]] virtual GraphicsResult Draw(const GraphicsPipeline& Pipeline, const Mesh& Geometry, const DrawArguments& Arguments,
			std::span<const BindingSet* const> BindingSets = {}) = 0;
		[[nodiscard]] virtual GraphicsResult DrawIndexed(const GraphicsPipeline& Pipeline, const Buffer& VertexBuffer,
			const Buffer& IndexBuffer, const DrawIndexedArguments& Arguments, std::span<const BindingSet* const> BindingSets = {}) = 0;
		[[nodiscard]] virtual GraphicsResult DrawIndexed(const GraphicsPipeline& Pipeline, const Mesh& Geometry,
			const DrawIndexedArguments& Arguments, std::span<const BindingSet* const> BindingSets = {}) = 0;

		[[nodiscard]] MeshCreateResult CreateMesh(const MeshDesc& Description);
	};
} // namespace PulseForge
