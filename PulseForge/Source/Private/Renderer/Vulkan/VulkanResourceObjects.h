#pragma once

#include "Renderer/RendererBackend.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>
#include <vector>

namespace PulseForge::VulkanDetail
{
	class VulkanBuffer final : public Buffer
	{
	public:
		VulkanBuffer(BufferDesc Description, nvrhi::BufferHandle Handle);
		[[nodiscard]] const BufferDesc& GetDescription() const noexcept override;
		[[nodiscard]] nvrhi::IBuffer* GetNativeBuffer() const noexcept;

	private:
		BufferDesc m_Description;
		nvrhi::BufferHandle m_Handle;
	};

	class VulkanTexture final : public Texture
	{
	public:
		VulkanTexture(TextureDesc Description, nvrhi::TextureHandle Handle);
		[[nodiscard]] const TextureDesc& GetDescription() const noexcept override;
		[[nodiscard]] nvrhi::ITexture* GetNativeTexture() const noexcept;
		[[nodiscard]] nvrhi::TextureHandle GetNativeTextureHandle() const noexcept;

	private:
		TextureDesc m_Description;
		nvrhi::TextureHandle m_Handle;
	};

	class VulkanRenderTarget final : public RenderTarget
	{
	public:
		VulkanRenderTarget(RenderTargetDesc Description, TextureHandle ColorTexture, TextureHandle DepthTexture,
			nvrhi::FramebufferHandle Framebuffer, const Texture* BorrowedDepthTexture = nullptr, uint32_t DepthArraySlice = 0);

		[[nodiscard]] const RenderTargetDesc& GetDescription() const noexcept override;
		[[nodiscard]] const Texture* GetColorTexture() const noexcept override;
		[[nodiscard]] const Texture* GetDepthTexture() const noexcept override;
		[[nodiscard]] nvrhi::FramebufferHandle GetNativeFramebuffer() const noexcept;
		[[nodiscard]] nvrhi::TextureHandle GetNativeColorTexture() const noexcept;
		[[nodiscard]] nvrhi::TextureHandle GetNativeDepthTexture() const noexcept;
		[[nodiscard]] nvrhi::TextureSubresourceSet GetDepthSubresources() const noexcept;

	private:
		// Declaration order ensures the framebuffer releases its attachment references first.
		RenderTargetDesc m_Description;
		TextureHandle m_ColorTexture;
		TextureHandle m_DepthTexture;
		const Texture* m_BorrowedDepthTexture = nullptr;
		uint32_t m_DepthArraySlice = 0;
		nvrhi::FramebufferHandle m_Framebuffer;
	};

	class VulkanDepthCubemap final : public DepthCubemap
	{
	public:
		VulkanDepthCubemap(TextureHandle Texture, uint32_t Resolution);
		[[nodiscard]] const Texture& GetTexture() const noexcept override;
		[[nodiscard]] const RenderTarget& GetFaceTarget(uint32_t Face) const override;
		[[nodiscard]] uint32_t GetResolution() const noexcept override;
		void SetFaceTarget(uint32_t Face, RenderTargetHandle Target);

	private:
		// Face targets release their framebuffer references before the cube texture wrapper.
		TextureHandle m_Texture;
		std::array<RenderTargetHandle, 6> m_FaceTargets;
		uint32_t m_Resolution = 0;
	};

	class VulkanSampler final : public Sampler
	{
	public:
		VulkanSampler(SamplerDesc Description, nvrhi::SamplerHandle Handle);
		[[nodiscard]] const SamplerDesc& GetDescription() const noexcept override;
		[[nodiscard]] nvrhi::ISampler* GetNativeSampler() const noexcept;

	private:
		SamplerDesc m_Description;
		nvrhi::SamplerHandle m_Handle;
	};

	class VulkanShader final : public Shader
	{
	public:
		VulkanShader(ShaderDesc Description, std::vector<std::byte> Bytecode, nvrhi::ShaderHandle Handle);
		[[nodiscard]] const ShaderDesc& GetDescription() const noexcept override;
		[[nodiscard]] nvrhi::IShader* GetNativeShader() const noexcept;

	private:
		ShaderDesc m_Description;
		std::vector<std::byte> m_Bytecode;
		nvrhi::ShaderHandle m_Handle;
	};

	class VulkanBindingLayout final : public BindingLayout
	{
	public:
		VulkanBindingLayout(BindingLayoutDesc Description, nvrhi::BindingLayoutHandle Handle);
		[[nodiscard]] const BindingLayoutDesc& GetDescription() const noexcept override;
		[[nodiscard]] nvrhi::IBindingLayout* GetNativeLayout() const noexcept;

	private:
		BindingLayoutDesc m_Description;
		nvrhi::BindingLayoutHandle m_Handle;
	};

	class VulkanBindingSet final : public BindingSet
	{
	public:
		VulkanBindingSet(BindingLayoutHandle Layout, nvrhi::BindingSetHandle Handle);
		[[nodiscard]] const BindingLayout& GetLayout() const noexcept override;
		[[nodiscard]] nvrhi::IBindingSet* GetNativeBindingSet() const noexcept;

	private:
		// NVRHI retains the native resources referenced by the set; this retains its engine layout.
		BindingLayoutHandle m_Layout;
		nvrhi::BindingSetHandle m_Handle;
	};

	class VulkanGraphicsPipeline final : public GraphicsPipeline
	{
	public:
		VulkanGraphicsPipeline(
			GraphicsPipelineDesc Description, nvrhi::InputLayoutHandle InputLayout, nvrhi::GraphicsPipelineHandle Handle);
		[[nodiscard]] const GraphicsPipelineDesc& GetDescription() const noexcept override;
		[[nodiscard]] nvrhi::IGraphicsPipeline* GetNativePipeline() const noexcept;

	private:
		// Reverse member destruction releases the pipeline before the input layout and shaders.
		GraphicsPipelineDesc m_Description;
		nvrhi::InputLayoutHandle m_InputLayout;
		nvrhi::GraphicsPipelineHandle m_Handle;
	};
} // namespace PulseForge::VulkanDetail
