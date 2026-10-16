#include "Renderer/Vulkan/VulkanResourceObjects.h"

#include <stdexcept>
#include <utility>

namespace PulseForge::VulkanDetail
{
	VulkanBuffer::VulkanBuffer(BufferDesc Description, nvrhi::BufferHandle Handle)
		: m_Description(std::move(Description)), m_Handle(std::move(Handle))
	{
	}

	const BufferDesc& VulkanBuffer::GetDescription() const noexcept
	{
		return m_Description;
	}

	nvrhi::IBuffer* VulkanBuffer::GetNativeBuffer() const noexcept
	{
		return m_Handle.Get();
	}

	VulkanTexture::VulkanTexture(TextureDesc Description, nvrhi::TextureHandle Handle)
		: m_Description(std::move(Description)), m_Handle(std::move(Handle))
	{
	}

	const TextureDesc& VulkanTexture::GetDescription() const noexcept
	{
		return m_Description;
	}

	nvrhi::ITexture* VulkanTexture::GetNativeTexture() const noexcept
	{
		return m_Handle.Get();
	}

	nvrhi::TextureHandle VulkanTexture::GetNativeTextureHandle() const noexcept
	{
		return m_Handle;
	}

	VulkanRenderTarget::VulkanRenderTarget(RenderTargetDesc Description, TextureHandle ColorTexture, TextureHandle DepthTexture,
		nvrhi::FramebufferHandle Framebuffer, const Texture* BorrowedDepthTexture, uint32_t DepthArraySlice)
		: m_Description(std::move(Description)), m_ColorTexture(std::move(ColorTexture)), m_DepthTexture(std::move(DepthTexture)),
		  m_BorrowedDepthTexture(BorrowedDepthTexture), m_DepthArraySlice(DepthArraySlice), m_Framebuffer(std::move(Framebuffer))
	{
	}

	const RenderTargetDesc& VulkanRenderTarget::GetDescription() const noexcept
	{
		return m_Description;
	}

	const Texture* VulkanRenderTarget::GetColorTexture() const noexcept
	{
		return m_ColorTexture.get();
	}

	const Texture* VulkanRenderTarget::GetDepthTexture() const noexcept
	{
		return m_DepthTexture ? m_DepthTexture.get() : m_BorrowedDepthTexture;
	}

	nvrhi::FramebufferHandle VulkanRenderTarget::GetNativeFramebuffer() const noexcept
	{
		return m_Framebuffer;
	}

	nvrhi::TextureHandle VulkanRenderTarget::GetNativeColorTexture() const noexcept
	{
		return m_ColorTexture ? static_cast<const VulkanTexture&>(*m_ColorTexture).GetNativeTextureHandle() : nvrhi::TextureHandle{};
	}

	nvrhi::TextureHandle VulkanRenderTarget::GetNativeDepthTexture() const noexcept
	{
		const Texture* DepthTexture = m_DepthTexture ? m_DepthTexture.get() : m_BorrowedDepthTexture;
		return DepthTexture ? static_cast<const VulkanTexture&>(*DepthTexture).GetNativeTextureHandle() : nvrhi::TextureHandle{};
	}

	nvrhi::TextureSubresourceSet VulkanRenderTarget::GetDepthSubresources() const noexcept
	{
		return m_BorrowedDepthTexture ? nvrhi::TextureSubresourceSet(0, 1, m_DepthArraySlice, 1) : nvrhi::TextureSubresourceSet();
	}

	VulkanDepthCubemap::VulkanDepthCubemap(TextureHandle Texture, uint32_t Resolution)
		: m_Texture(std::move(Texture)), m_Resolution(Resolution)
	{
	}

	const Texture& VulkanDepthCubemap::GetTexture() const noexcept
	{
		return *m_Texture;
	}

	const RenderTarget& VulkanDepthCubemap::GetFaceTarget(uint32_t Face) const
	{
		if (Face >= m_FaceTargets.size() || !m_FaceTargets[Face]) throw std::out_of_range("Depth cubemap face index is not available");
		return *m_FaceTargets[Face];
	}

	uint32_t VulkanDepthCubemap::GetResolution() const noexcept
	{
		return m_Resolution;
	}

	void VulkanDepthCubemap::SetFaceTarget(uint32_t Face, RenderTargetHandle Target)
	{
		m_FaceTargets.at(Face) = std::move(Target);
	}

	VulkanSampler::VulkanSampler(SamplerDesc Description, nvrhi::SamplerHandle Handle)
		: m_Description(std::move(Description)), m_Handle(std::move(Handle))
	{
	}

	const SamplerDesc& VulkanSampler::GetDescription() const noexcept
	{
		return m_Description;
	}

	nvrhi::ISampler* VulkanSampler::GetNativeSampler() const noexcept
	{
		return m_Handle.Get();
	}

	VulkanShader::VulkanShader(ShaderDesc Description, std::vector<std::byte> Bytecode, nvrhi::ShaderHandle Handle)
		: m_Description(std::move(Description)), m_Bytecode(std::move(Bytecode)), m_Handle(std::move(Handle))
	{
	}

	const ShaderDesc& VulkanShader::GetDescription() const noexcept
	{
		return m_Description;
	}

	nvrhi::IShader* VulkanShader::GetNativeShader() const noexcept
	{
		return m_Handle.Get();
	}

	VulkanBindingLayout::VulkanBindingLayout(BindingLayoutDesc Description, nvrhi::BindingLayoutHandle Handle)
		: m_Description(std::move(Description)), m_Handle(std::move(Handle))
	{
	}

	const BindingLayoutDesc& VulkanBindingLayout::GetDescription() const noexcept
	{
		return m_Description;
	}

	nvrhi::IBindingLayout* VulkanBindingLayout::GetNativeLayout() const noexcept
	{
		return m_Handle.Get();
	}

	VulkanBindingSet::VulkanBindingSet(BindingLayoutHandle Layout, nvrhi::BindingSetHandle Handle)
		: m_Layout(std::move(Layout)), m_Handle(std::move(Handle))
	{
	}

	const BindingLayout& VulkanBindingSet::GetLayout() const noexcept
	{
		return *m_Layout;
	}

	nvrhi::IBindingSet* VulkanBindingSet::GetNativeBindingSet() const noexcept
	{
		return m_Handle.Get();
	}

	VulkanGraphicsPipeline::VulkanGraphicsPipeline(
		GraphicsPipelineDesc Description, nvrhi::InputLayoutHandle InputLayout, nvrhi::GraphicsPipelineHandle Handle)
		: m_Description(std::move(Description)), m_InputLayout(std::move(InputLayout)), m_Handle(std::move(Handle))
	{
	}

	const GraphicsPipelineDesc& VulkanGraphicsPipeline::GetDescription() const noexcept
	{
		return m_Description;
	}

	nvrhi::IGraphicsPipeline* VulkanGraphicsPipeline::GetNativePipeline() const noexcept
	{
		return m_Handle.Get();
	}
} // namespace PulseForge::VulkanDetail
